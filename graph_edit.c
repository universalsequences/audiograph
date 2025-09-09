#include "graph_edit.h"
#include "hot_swap.h"

// to be called from block-boundary (i.e. before each block is executed)
bool apply_graph_edits(GraphEditQueue *r, LiveGraph *lg) {
  GraphEditCmd cmd;

  const int MAX_CMDS_PER_BLOCK = 0;
  int applied = 0;
  bool all_ok = true;

  while (geq_pop(r, &cmd)) {
    if (MAX_CMDS_PER_BLOCK && applied >= MAX_CMDS_PER_BLOCK) {
      break;
    }

    bool ok = true;

    // then we have a cmd to run
    switch (cmd.op) {
    case GE_ADD_NODE: {
      int nid = apply_add_node(lg, cmd.u.add_node.vt, cmd.u.add_node.state_size,
                               cmd.u.add_node.logical_id, cmd.u.add_node.name,
                               cmd.u.add_node.nInputs, cmd.u.add_node.nOutputs);
      ok = nid >= 0;
      if (!ok) {
        // Track the failed logical ID
        add_failed_id(lg, cmd.u.add_node.logical_id);
      }
      break;
    }
    case GE_REMOVE_NODE:
      ok = apply_delete_node(lg, cmd.u.remove_node.node_id);
      break;
    case GE_CONNECT: {
      ok = apply_connect(lg, cmd.u.connect.src_id, cmd.u.connect.src_port,
                         cmd.u.connect.dst_id, cmd.u.connect.dst_port);
      break;
    }
    case GE_DISCONNECT: {
      ok = apply_disconnect(lg, cmd.u.disconnect.src_id,
                            cmd.u.disconnect.src_port, cmd.u.disconnect.dst_id,
                            cmd.u.disconnect.dst_port);
      break;
    }
    case GE_HOT_SWAP_NODE:
      ok = apply_hot_swap(lg, &cmd.u.hot_swap_node);
      break;
    case GE_REPLACE_KEEP_EDGES:
      ok = apply_replace_keep_edges(lg, &cmd.u.replace_keep_edges);
      break;
    default: {
      ok = false; // unknown op
      break;
    }
    }
    applied++;
    if (!ok) {
      all_ok = false;
    }
  }

  // Debug invariant checking (only in debug builds)
  /*
#ifndef NDEBUG
  extern void assert_unique_pred_invariants(LiveGraph *lg);
  assert_unique_pred_invariants(lg);
#endif
  */

  return all_ok;
}
