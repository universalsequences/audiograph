#include "hot_swap.h"

// ports.c

static int highest_connected_input_index(const RTNode *n) {
  if (!n->inEdgeId)
    return -1;
  int hi = -1;
  for (int i = 0; i < n->nInputs; i++)
    if (n->inEdgeId[i] >= 0)
      hi = i;
  return hi;
}

static int highest_connected_output_index(const RTNode *n) {
  if (!n->outEdgeId)
    return -1;
  int hi = -1;
  for (int i = 0; i < n->nOutputs; i++)
    if (n->outEdgeId[i] >= 0)
      hi = i;
  return hi;
}

// Grow arrays preserving existing connections; initialize new slots to -1.
static bool resize_ports_preserve(RTNode *n, int nin, int nout) {
  // Inputs
  if (nin != n->nInputs) {
    int32_t *new_in = NULL;
    int32_t *new_fanin = NULL;
    if (nin > 0) {
      new_in = realloc(n->inEdgeId, sizeof(int32_t) * nin);
      if (!new_in)
        return false;
      for (int i = n->nInputs; i < nin; i++)
        new_in[i] = -1;

      new_fanin = realloc(n->fanin_sum_node_id, sizeof(int32_t) * nin);
      if (!new_fanin)
        return false;
      for (int i = n->nInputs; i < nin; i++)
        new_fanin[i] = -1;
    } else {
      free(n->inEdgeId);
      new_in = NULL;
      free(n->fanin_sum_node_id);
      new_fanin = NULL;
    }
    n->inEdgeId = new_in;
    n->fanin_sum_node_id = new_fanin;
    n->nInputs = nin;
  }

  // Outputs
  if (nout != n->nOutputs) {
    int32_t *new_out = NULL;
    if (nout > 0) {
      new_out = realloc(n->outEdgeId, sizeof(int32_t) * nout);
      if (!new_out)
        return false;
      for (int i = n->nOutputs; i < nout; i++)
        new_out[i] = -1;
    } else {
      free(n->outEdgeId);
      new_out = NULL;
    }
    n->outEdgeId = new_out;
    n->nOutputs = nout;
  }

  return true;
}

static bool ports_are_compatible(const RTNode *n, int nin, int nout) {
  int hi_in = highest_connected_input_index(n);
  int hi_out = highest_connected_output_index(n);
  return (nin > hi_in) && (nout > hi_out);
}

bool apply_hot_swap(LiveGraph *lg, GEHotSwapNode *p) {
  int id = p->node_id;
  if (id >= lg->node_count || lg->dac_node_id == id || id < 0) {
    // node does not exist / is dac (which can't be hotswapped)
    return false;
  }

  RTNode *n = &lg->nodes[id];

  bool deleted =
      n->vtable.process == NULL && n->nInputs == 0 && n->nOutputs == 0;

  if (deleted)
    return false;

  // Must be port-compatible
  if (!ports_are_compatible(n, p->new_nInputs, p->new_nOutputs))
    return false;

  // Grow ports if needed; never shrink below active indices (replace_keep_edges
  // handles that more complex case)
  if (!resize_ports_preserve(n, p->new_nInputs, p->new_nOutputs))
    return false;

  void *old_state = n->state;

  n->state = p->state;
  n->vtable = p->vt;

  retire_later(lg, old_state, free);

  return true;
}

bool apply_replace_keep_edges(LiveGraph *lg, GEReplaceKeepEdges *p) {
  int id = p->node_id;
  if (id >= lg->node_count || lg->dac_node_id == id || id < 0) {
    // node does not exist / is dac (which can't be hotswapped)
    return false;
  }

  RTNode *n = &lg->nodes[id];

  bool deleted =
      n->vtable.process == NULL && n->nInputs == 0 && n->nOutputs == 0;

  if (deleted)
    return false;

  int old_nin = n->nInputs;
  int old_nout = n->nOutputs;

  // 1) If shrinking inputs, auto-disconnect ports [p->new_nInputs .. old_nin-1]
  if (p->new_nInputs < old_nin) {
    for (int di = p->new_nInputs; di < old_nin; di++) {
      int eid = n->inEdgeId[di];
      if (eid >= 0) {
        int src_id = lg->edges[eid].src_node;
        int src_port = lg->edges[eid].src_port;
        if (src_id >= 0 && src_port >= 0) {
          apply_disconnect(lg, src_id, src_port, id, di);
        }
      }
    }
  }

  // 2) If shrinking outputs, auto-disconnect all consumers of ports
  // [p->new_nOutputs .. old_nout-1]
  if (p->new_nOutputs < old_nout) {
    for (int sp = p->new_nOutputs; sp < old_nout; sp++) {
      int eid = n->outEdgeId[sp];
      if (eid < 0)
        continue;

      // Find and clear all destinations consuming this edge (use existing
      // pattern from apply_delete_node)
      for (int dst = 0; dst < lg->node_count; dst++) {
        if (dst == id)
          continue;
        RTNode *D = &lg->nodes[dst];
        if (!D->inEdgeId || D->nInputs <= 0)
          continue;
        for (int di = 0; di < D->nInputs; di++) {
          if (D->inEdgeId[di] == eid) {
            // we found the edge to delete (so we  disconnect these two
            // nodes+ports)
            apply_disconnect(lg, id, sp, dst, di);
          }
        }
      }
    }
  }

  // 3) Resize port arrays (grow/shrink). Existing low-index connections
  // preserved by resize_ports_preserve
  if (!resize_ports_preserve(n, p->new_nInputs, p->new_nOutputs))
    return false;

  // 4) Install new state/vtable
  void *old_state = n->state;

  n->state = p->state;
  n->vtable = p->vt;

  retire_later(lg, old_state, free);

  // Topology changed (some ports disconnected), so refresh orphan flags
  update_orphaned_status(lg);

  // After hot swap replacement, ensure indegree matches actual unique predecessors
  // This fixes edge cases where port shrinking can leave indegree inconsistent
  int actual_unique_preds = 0;
  bool seen[8192] = {0}; // Assume max nodes < 8192
  if (n->inEdgeId) {
    for (int di = 0; di < n->nInputs; di++) {
      int eid = n->inEdgeId[di];
      if (eid < 0) continue;
      int s = lg->edges[eid].src_node;
      if (s >= 0 && s < 8192 && !seen[s]) { 
        seen[s] = true; 
        actual_unique_preds++; 
      }
    }
  }
  lg->indegree[id] = actual_unique_preds;

  return true;
}
