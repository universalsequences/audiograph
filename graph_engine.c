#include "graph_engine.h"
#include "graph_edit.h"
#include "graph_nodes.h"

// ===================== Forward Declarations =====================

void bind_and_run_live(LiveGraph *lg, int nid, int nframes);
static void ensure_port_arrays(RTNode *n);
static void init_pending_and_seed(LiveGraph *lg);
static int alloc_edge(LiveGraph *lg);
void process_live_block(LiveGraph *lg, int nframes);

// ===================== Global Engine Instance =====================

Engine g_engine; // single global for demo

void initialize_engine(int block_Size, int sample_rate) {
  g_engine.blockSize = block_Size;
  g_engine.sampleRate = sample_rate;
}

// ===================== Graph Management =====================

// ===================== Parameter Application =====================

void apply_params(LiveGraph *g) {
  if (!g || !g->params)
    return;
  ParamMsg m;
  while (params_pop(g->params, &m)) {
    for (int i = 0; i < g->node_count; i++) {
      if (g->nodes[i].logical_id == m.logical_id) {
        if (g->nodes[i].state) { // Only apply if node has memory
          float *memory = (float *)g->nodes[i].state;
          memory[m.idx] = m.fvalue; // Direct indexed access
        }
      }
    }
  }
}

// ===================== Block Processing =====================

// Legacy bind_and_run function removed - using port-based bind_and_run_live
// only

static void *worker_main(void *arg) {
  (void)arg;
  for (;;) {
    if (!atomic_load_explicit(&g_engine.runFlag, memory_order_acquire))
      break;

    LiveGraph *lg =
        atomic_load_explicit(&g_engine.workSession, memory_order_acquire);
    if (!lg) {
      sched_yield();
      continue;
    }

    int32_t nid;
    // Try to get a ready node from the live graph's MPMC queue
    if (mpmc_pop(lg->readyQueue, &nid)) {
      pthread_t thread_id = pthread_self();
      printf("    [LIVE_WORKER %lu] Processing node_id=%d for %d frames\n",
             (unsigned long)thread_id, nid, g_engine.blockSize);

      bind_and_run_live(lg, nid, g_engine.blockSize);
      RTNode *node = &lg->nodes[nid];

      // Notify successors
      if (node->succ && node->succCount > 0) {
        for (int i = 0; i < node->succCount; i++) {
          int succ = node->succ[i];
          if (succ >= 0 && succ < lg->node_count && !lg->is_orphaned[succ]) {
            if (atomic_fetch_sub_explicit(&lg->pending[succ], 1,
                                          memory_order_acq_rel) == 1) {
              while (!mpmc_push(lg->readyQueue, succ)) {
                __asm__ __volatile__("" ::: "memory");
              }
            }
          }
        }
      }
      atomic_fetch_sub_explicit(&lg->jobsInFlight, 1, memory_order_acq_rel);
      printf("    [LIVE_WORKER %lu] Completed node_id=%d\n",
             (unsigned long)thread_id, nid);
    } else {
      // Queue is empty - use hybrid spin-yield strategy:

      // First, do a brief spin loop (64 iterations) to avoid syscall overhead
      // if work becomes available soon. The memory barrier prevents compiler
      // optimization from removing the loop.
      for (int i = 0; i < 64; i++) {
        __asm__ __volatile__(
            "" ::
                : "memory"); // Memory barrier (no-op instruction)
      }

      // Still no work - cooperatively yield CPU time to other threads
      // (audio thread, other workers, or OS tasks)
      sched_yield();

      // Go back to top of worker loop to check for new work
      continue;
    }
  }
  return NULL;
}

// ===================== Worker Pool Management =====================

void engine_start_workers(int workers) {
  g_engine.workerCount = workers;
  g_engine.threads = (pthread_t *)calloc(workers, sizeof(pthread_t));
  atomic_store(&g_engine.runFlag, 1);
  for (int i = 0; i < workers; i++) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create(&g_engine.threads[i], &attr, worker_main, NULL);
    pthread_attr_destroy(&attr);
  }
}

void engine_stop_workers(void) {
  atomic_store(&g_engine.runFlag, 0);
  for (int i = 0; i < g_engine.workerCount; i++) {
    pthread_join(g_engine.threads[i], NULL);
  }
  free(g_engine.threads);
  g_engine.threads = NULL;
  g_engine.workerCount = 0;
}

// ===================== Live Graph Operations =====================

static uint64_t g_next_node_id = 0x1000;

// Ensure node's port arrays exist/are sized (call at node creation in practice)
static void ensure_port_arrays(RTNode *n) {
  if (!n->inEdgeId && n->nInputs > 0) {
    n->inEdgeId = (int32_t *)malloc(sizeof(int32_t) * n->nInputs);
    for (int i = 0; i < n->nInputs; i++)
      n->inEdgeId[i] = -1;
  }
  if (!n->outEdgeId && n->nOutputs > 0) {
    n->outEdgeId = (int32_t *)malloc(sizeof(int32_t) * n->nOutputs);
    for (int i = 0; i < n->nOutputs; i++)
      n->outEdgeId[i] = -1;
  }
}

LiveGraph *create_live_graph(int initial_capacity, int block_size,
                             const char *label) {
  LiveGraph *lg = calloc(1, sizeof(LiveGraph));

  // Node storage
  lg->node_capacity = initial_capacity;
  lg->nodes = calloc(lg->node_capacity, sizeof(RTNode));
  lg->pending = calloc(lg->node_capacity, sizeof(atomic_int));
  lg->indegree = calloc(lg->node_capacity, sizeof(int));
  lg->is_orphaned = calloc(lg->node_capacity, sizeof(bool));

  // Edge pool (start with generous capacity)
  lg->edge_capacity = initial_capacity * 4;
  lg->block_size = block_size;

  // New port-based edge pool
  lg->edges = calloc(lg->edge_capacity, sizeof(LiveEdge));
  for (int i = 0; i < lg->edge_capacity; i++) {
    lg->edges[i].buf = alloc_aligned(64, block_size * sizeof(float));
    lg->edges[i].in_use = false;
    lg->edges[i].refcount = 0;
  }

  // Support buffers for port system
  lg->silence_buf = alloc_aligned(64, block_size * sizeof(float));
  lg->scratch_null = alloc_aligned(64, block_size * sizeof(float));
  memset(lg->silence_buf, 0,
         block_size * sizeof(float)); // keep silence buffer zeroed

  // Connection tracking
  lg->connection_capacity = initial_capacity * 8;
  lg->connections = calloc(lg->connection_capacity, sizeof(LiveConnection));

  // Ready queue (MPMC for thread safety)
  lg->readyQueue = mpmc_create(1024);
  if (!lg->readyQueue) {
    // Handle allocation failure - clean up port-based edges
    for (int i = 0; i < lg->edge_capacity; i++) {
      free(lg->edges[i].buf);
    }
    free(lg->edges);
    free(lg->silence_buf);
    free(lg->scratch_null);
    free(lg->connections);
    free(lg->nodes);
    free(lg->pending);
    free(lg->indegree);
    free(lg->is_orphaned);
    free(lg);
    return NULL;
  }

  // Parameter mailbox
  lg->params = calloc(1, sizeof(ParamRing));

  lg->graphEditQueue = calloc(1, sizeof(GraphEditQueue));
  geq_init(lg->graphEditQueue, 256);
  
  // Initialize failed IDs tracking
  lg->failed_ids_capacity = 64; // Start with reasonable capacity
  lg->failed_ids = calloc(lg->failed_ids_capacity, sizeof(uint64_t));
  lg->failed_ids_count = 0;
  
  // Initialize atomic node ID counter (start at 1 to avoid confusion with DAC at 0)
  atomic_init(&lg->next_node_id, 1);

  // Automatically create the DAC node at index 0
  int dac_id = apply_add_node(lg, DAC_VTABLE, NULL, 0, "DAC", 1, 1);
  if (dac_id >= 0) {
    lg->dac_node_id = dac_id; // Remember the DAC node
    
    RTNode *dac = &lg->nodes[dac_id];
    dac->nInputs = 1;  // DAC has 1 input (audio in)
    dac->nOutputs = 1; // DAC has 1 output (for reading final audio)
    ensure_port_arrays(dac);
    
    // Allocate an output edge for the DAC so we can read the final audio
    int output_edge = alloc_edge(lg);
    if (output_edge >= 0) {
      dac->outEdgeId[0] = output_edge; // Use port-based system
    }
  }

  lg->label = label;
  return lg;
}

// Legacy edge functions removed - using port-based alloc_edge instead

int apply_add_node(LiveGraph *lg, NodeVTable vtable, void *state,
                   uint64_t logical_id, const char *name, int nInputs, int nOutputs) {
  // Use logical_id directly as the array index
  int node_id = (int)logical_id;
  if (node_id >= lg->node_capacity) {
    // Need to expand - for demo just fail
    return -1;
  }

  RTNode *node = &lg->nodes[node_id];
  memset(node, 0, sizeof(RTNode));

  node->logical_id = logical_id;
  node->vtable = vtable;
  node->state = state;
  node->faninBase = 0;
  node->succCount = 0;

  // Set port counts from command
  node->nInputs = nInputs;
  node->nOutputs = nOutputs;
  
  // Initialize port arrays to NULL first
  node->inEdgeId = NULL;
  node->outEdgeId = NULL;
  node->succ = NULL;
  
  // Set up port arrays if needed
  ensure_port_arrays(node);

  // Initialize orphaned state - new nodes with no connections start as orphaned
  // They will be marked as non-orphaned when they get connected to the signal
  // path
  lg->is_orphaned[node_id] = true;

  // Update node_count to be highest allocated index + 1
  if (node_id >= lg->node_count) {
    lg->node_count = node_id + 1;
  }

  return node_id;
}

int live_add_oscillator(LiveGraph *lg, float freq_hz, const char *name) {
  float *memory = calloc(OSC_MEMORY_SIZE, sizeof(float));
  memory[OSC_PHASE] = 0.0f;
  memory[OSC_INC] = freq_hz / 48000.0f;
  int node_id = apply_add_node(lg, OSC_VTABLE, memory, ++g_next_node_id, name, 0, 1);
  if (node_id >= 0) {
    RTNode *node = &lg->nodes[node_id];
    node->nInputs = 0;  // Oscillator has no inputs
    node->nOutputs = 1; // Oscillator has 1 output
    ensure_port_arrays(node);
  }
  return node_id;
}

int live_add_gain(LiveGraph *lg, float gain_value, const char *name) {
  float *memory = calloc(GAIN_MEMORY_SIZE, sizeof(float));
  memory[GAIN_VALUE] = gain_value;
  int node_id = apply_add_node(lg, GAIN_VTABLE, memory, ++g_next_node_id, name, 1, 1);
  if (node_id >= 0) {
    RTNode *node = &lg->nodes[node_id];
    node->nInputs = 1;  // Gain has 1 input
    node->nOutputs = 1; // Gain has 1 output
    ensure_port_arrays(node);
  }
  return node_id;
}

int live_add_mixer2(LiveGraph *lg, const char *name) {
  int node_id = apply_add_node(lg, MIX2_VTABLE, NULL, ++g_next_node_id, name, 2, 1);
  if (node_id >= 0) {
    RTNode *node = &lg->nodes[node_id];
    node->nInputs = 2;  // Mix2 has 2 inputs
    node->nOutputs = 1; // Mix2 has 1 output
    ensure_port_arrays(node);
  }
  return node_id;
}

int live_add_mixer8(LiveGraph *lg, const char *name) {
  int node_id = apply_add_node(lg, MIX8_VTABLE, NULL, ++g_next_node_id, name, 8, 1);
  if (node_id >= 0) {
    RTNode *node = &lg->nodes[node_id];
    node->nInputs = 8;  // Mix8 has 8 inputs
    node->nOutputs = 1; // Mix8 has 1 output
    ensure_port_arrays(node);
  }
  return node_id;
}

// DAC function moved after helper function declarations

// Port-based successor management
static void add_successor(RTNode *node, int succ_id) {
  // Expand successor array
  node->succ = realloc(node->succ, (node->succCount + 1) * sizeof(int32_t));
  node->succ[node->succCount] = succ_id;
  node->succCount++;
}

static void remove_successor(RTNode *src, int succ_id) {
  for (int i = 0; i < src->succCount; i++) {
    if (src->succ[i] == succ_id) {
      // swap-with-last
      int last = src->succCount - 1;
      if (i != last)
        src->succ[i] = src->succ[last];
      src->succCount--;
      if (src->succCount == 0) {
        free(src->succ);
        src->succ = NULL;
      } else {
        src->succ =
            (int32_t *)realloc(src->succ, sizeof(int32_t) * src->succCount);
      }
      return;
    }
  }
}

static void retire_edge(LiveGraph *lg, int eid) {
  if (eid < 0 || eid >= lg->edge_capacity)
    return;
  // zero for hygiene (optional)
  if (lg->edges[eid].buf) {
    memset(lg->edges[eid].buf, 0, sizeof(float) * lg->block_size);
    free(lg->edges[eid].buf);
    lg->edges[eid].buf = NULL;
  }
  lg->edges[eid].refcount = 0;
  lg->edges[eid].in_use = false;
}

// ===================== New Port-Based System Functions =====================

// Add successor (swap-with-last removal used on disconnect)
static inline void add_successor_port(RTNode *src, int succ_id) {
  src->succ =
      (int32_t *)realloc(src->succ, sizeof(int32_t) * (src->succCount + 1));
  src->succ[src->succCount++] = succ_id;
}

// Optional: check if successor already present (prevent dup edges in succ list)
static inline bool has_successor(const RTNode *src, int succ_id) {
  for (int i = 0; i < src->succCount; i++)
    if (src->succ[i] == succ_id)
      return true;
  return false;
}

// Allocate (or reuse from pool) an edge buffer; returns edge id or -1
static int alloc_edge(LiveGraph *lg) {
  for (int i = 0; i < lg->edge_capacity; i++) {
    if (!lg->edges[i].in_use) {
      lg->edges[i].in_use = true;
      lg->edges[i].refcount = 0;
      // Allocate buffer if it was freed during retire_edge
      if (!lg->edges[i].buf) {
        lg->edges[i].buf = alloc_aligned(64, lg->block_size * sizeof(float));
      }
      // Zero buffer for safety
      memset(lg->edges[i].buf, 0, sizeof(float) * lg->block_size);
      return i;
    }
  }
  return -1; // pool exhausted; grow in apply_graph_edits if you want
}

// Check if a node has any connected outputs (for scheduling)
static inline bool node_has_any_output_connected(LiveGraph *lg, int node_id) {
  RTNode *node = &lg->nodes[node_id];
  if (!node->outEdgeId)
    return false;

  for (int i = 0; i < node->nOutputs; i++) {
    if (node->outEdgeId[i] >= 0)
      return true;
  }
  return false;
}

// Recursive function to mark nodes reachable from DAC (port-based only)
static void mark_reachable_from_dac(LiveGraph *lg, int node_id, bool *visited) {
  if (node_id < 0 || node_id >= lg->node_count || visited[node_id]) {
    return;
  }

  visited[node_id] = true;
  lg->is_orphaned[node_id] = false; // Mark as not orphaned

  RTNode *node = &lg->nodes[node_id];

  // Traverse backwards through input edges to find all nodes that feed this one
  // Port-based system only - all nodes should have port arrays initialized
  if (!node->inEdgeId || node->nInputs <= 0)
    return;

  for (int i = 0; i < node->nInputs; i++) {
    int edge_id = node->inEdgeId[i];
    if (edge_id < 0)
      continue; // unconnected port

    // Find the source node that outputs to this edge
    for (int j = 0; j < lg->node_count; j++) {
      RTNode *potential_source = &lg->nodes[j];
      if (!potential_source->outEdgeId || potential_source->nOutputs <= 0)
        continue;

      for (int k = 0; k < potential_source->nOutputs; k++) {
        if (potential_source->outEdgeId[k] == edge_id) {
          mark_reachable_from_dac(lg, j, visited);
          break;
        }
      }
    }
  }
}

// Update orphaned status for all nodes based on DAC reachability
static void update_orphaned_status(LiveGraph *lg) {
  // First, mark all nodes as orphaned
  for (int i = 0; i < lg->node_count; i++) {
    lg->is_orphaned[i] = true;
  }

  // If no DAC node exists, all nodes remain orphaned
  if (lg->dac_node_id < 0) {
    return;
  }

  // Use DFS to mark all nodes reachable from DAC
  bool *visited = calloc(lg->node_count, sizeof(bool));
  mark_reachable_from_dac(lg, lg->dac_node_id, visited);
  free(visited);
}

/**
 * Port-mapped connect:
 *   - One producer per dst input port
 *   - One edge buffer per src output port (shared by all consumers)
 *
 * Returns false on invalid params or capacity issues.
 *
 * NOTE: Call this from your *block-boundary* edit applier (not from RT
 * threads).
 */
bool apply_connect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                   int dst_port) {
  // --- Validate nodes/ports ---
  if (!lg || src_node < 0 || src_node >= lg->node_count || dst_node < 0 ||
      dst_node >= lg->node_count)
    return false;

  RTNode *S = &lg->nodes[src_node];
  RTNode *D = &lg->nodes[dst_node];

  if (src_port < 0 || src_port >= S->nOutputs || dst_port < 0 ||
      dst_port >= D->nInputs)
    return false;

  ensure_port_arrays(S);
  ensure_port_arrays(D);

  // Reject multiple producers per input port (enforce 1:1)
  if (D->inEdgeId[dst_port] != -1) {
    // Either reject or auto-disconnect existing; we reject:
    return false;
  }

  // --- Ensure the source output port has an edge id (alloc if absent) ---
  int eid = S->outEdgeId[src_port];
  if (eid == -1) {
    eid = alloc_edge(lg);
    if (eid < 0)
      return false; // no capacity
    S->outEdgeId[src_port] = eid;
  }

  // --- Wire consumer ---
  D->inEdgeId[dst_port] = eid;
  lg->edges[eid].refcount++;

  // --- Scheduling bookkeeping ---
  // indegree = number of connected input ports
  lg->indegree[dst_node]++;

  // successors (node-level is sufficient for scheduling)
  if (!has_successor(S, dst_node)) {
    add_successor_port(S, dst_node);
  }

  // Update orphaned status based on DAC reachability
  update_orphaned_status(lg);

  return true;
}


/**
 * Disconnect exactly one destination input port from a source output port.
 * Idempotent: returns true if the specific mapping existed and was removed;
 * returns false for invalid params or mismatched mapping.
 */
bool apply_disconnect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                      int dst_port) {
  if (!lg || src_node < 0 || src_node >= lg->node_count || dst_node < 0 ||
      dst_node >= lg->node_count) {
    return false;
  }

  RTNode *S = &lg->nodes[src_node];
  RTNode *D = &lg->nodes[dst_node];

  if (src_port < 0 || src_port >= S->nOutputs || dst_port < 0 ||
      dst_port >= D->nInputs) {
    return false;
  }

  if (!D->inEdgeId || !S->outEdgeId)
    return false;

  int eid_in = D->inEdgeId[dst_port];
  int eid_out = S->outEdgeId[src_port];

  // Nothing connected on that dst port → nothing to do (idempotent false)
  if (eid_in < 0)
    return false;

  // Ensure we're disconnecting the intended link: dst_port must be fed by
  // src_port
  if (eid_out < 0 || eid_in != eid_out) {
    // Either the dst input is driven by a different source port,
    // or the source port has no edge: treat as mismatch.
    return false;
  }

  // 1) Unwire the destination port
  D->inEdgeId[dst_port] = -1;

  // 2) Scheduling bookkeeping - update indegree for orphan detection
  if (lg->indegree) {
    lg->indegree[dst_node]--;
  }
  D->faninBase--; // fewer dependencies

  // 3) Update successor list on the source node
  remove_successor(S, dst_node);

  // 4) Edge refcount and retirement if last consumer
  LiveEdge *e = &lg->edges[eid_in];
  if (e->refcount > 0)
    e->refcount--;
  if (e->refcount == 0) {
    // No more consumers of this source port's signal
    retire_edge(lg, eid_in);
    S->outEdgeId[src_port] = -1;
  }

  // 5) Update orphaned status based on DAC reachability
  update_orphaned_status(lg);

  printf("Disconnected node %d:%d -> node %d:%d (edge %d)\n", src_node,
         src_port, dst_node, dst_port, eid_in);
  return true;
}

/**
 * Delete a node from the live graph, properly disconnecting all its
 * connections. This function:
 * 1. Disconnects all inbound connections to this node
 * 2. Disconnects all outbound connections from this node
 * 3. Frees the node's state memory and port arrays
 * 4. Updates orphaned status for the graph
 * Returns true if successful, false if node_id is invalid.
 */
bool apply_delete_node(LiveGraph *lg, int node_id) {
  if (!lg || node_id < 0 || node_id >= lg->node_count) {
    return false;
  }

  RTNode *node = &lg->nodes[node_id];

  // Special handling for DAC node
  if (lg->dac_node_id == node_id) {
    lg->dac_node_id = -1; // Clear DAC reference
  }

  // 1) Disconnect all inbound connections (clean up other nodes' outputs to
  // this node)
  if (node->inEdgeId && node->nInputs > 0) {
    for (int dst_port = 0; dst_port < node->nInputs; dst_port++) {
      int edge_id = node->inEdgeId[dst_port];
      if (edge_id < 0)
        continue; // Port not connected

      // Find and clear the source node's output port that feeds this input
      for (int src_node = 0; src_node < lg->node_count; src_node++) {
        if (src_node == node_id)
          continue; // Skip self
        RTNode *src = &lg->nodes[src_node];
        if (!src->outEdgeId || src->nOutputs <= 0)
          continue;

        for (int src_port = 0; src_port < src->nOutputs; src_port++) {
          if (src->outEdgeId[src_port] == edge_id) {
            // Clear the source's output port and remove successor
            src->outEdgeId[src_port] = -1;
            remove_successor(src, node_id);

            // Decrease edge refcount - retire_edge will be called by the port
            // cleanup below
            break;
          }
        }
      }
      node->inEdgeId[dst_port] = -1; // Clear this node's input
    }
  }

  // 2) Disconnect all outbound connections (clean up other nodes' inputs from
  // this node)
  if (node->outEdgeId && node->nOutputs > 0) {
    for (int src_port = 0; src_port < node->nOutputs; src_port++) {
      int edge_id = node->outEdgeId[src_port];
      if (edge_id < 0)
        continue; // Port not connected

      // Find and clear destination nodes' input ports that consume this output
      for (int dst_node = 0; dst_node < lg->node_count; dst_node++) {
        if (dst_node == node_id)
          continue; // Skip self
        RTNode *dst = &lg->nodes[dst_node];
        if (!dst->inEdgeId || dst->nInputs <= 0)
          continue;

        for (int dst_port = 0; dst_port < dst->nInputs; dst_port++) {
          if (dst->inEdgeId[dst_port] == edge_id) {
            // Clear the destination's input port and update scheduling
            dst->inEdgeId[dst_port] = -1;
            if (lg->indegree)
              lg->indegree[dst_node]--;
            dst->faninBase--;
          }
        }
      }

      // Decrease edge refcount and retire if zero
      LiveEdge *e = &lg->edges[edge_id];
      if (e->refcount > 0)
        e->refcount--;
      if (e->refcount == 0) {
        retire_edge(lg, edge_id);
      }
      node->outEdgeId[src_port] = -1; // Clear this node's output
    }
  }

  // 3) Free node's memory
  if (node->state) {
    free(node->state);
    node->state = NULL;
  }

  // Free port arrays
  if (node->inEdgeId) {
    free(node->inEdgeId);
    node->inEdgeId = NULL;
  }
  if (node->outEdgeId) {
    free(node->outEdgeId);
    node->outEdgeId = NULL;
  }
  if (node->succ) {
    free(node->succ);
    node->succ = NULL;
  }

  // 4) Clear node data and mark as deleted
  // Note: Don't clear logical_id since it's now the array index
  node->state = NULL;      // Mark as deleted (state is freed above)
  memset(&node->vtable, 0, sizeof(NodeVTable)); // Clear vtable
  node->nInputs = 0;
  node->nOutputs = 0;

  // Note: We don't compact the node array to maintain stable node IDs
  // The slot can be reused by apply_add_node if needed

  // 5) Update orphaned status for the entire graph
  update_orphaned_status(lg);

  printf("Deleted node %d and all its connections\n", node_id);
  return true;
}

void bind_and_run_live(LiveGraph *lg, int nid, int nframes) {
  RTNode *node = &lg->nodes[nid];
  
  // Safety checks for deleted/invalid nodes
  if (node->state == NULL)    // Node was deleted
    return;
  if (lg->is_orphaned[nid])   // Node is orphaned
    return;
  if (node->nInputs < 0 || node->nOutputs < 0)  // Invalid port counts
    return;

  float *inPtrs[MAX_IO];
  float *outPtrs[MAX_IO];

  // Inputs: each port has 0/1 producer (edge id or -1)
  for (int i = 0; i < node->nInputs && i < MAX_IO; i++) {
    int eid = node->inEdgeId ? node->inEdgeId[i] : -1;
    inPtrs[i] = (eid >= 0) ? lg->edges[eid].buf : lg->silence_buf;
  }

  // Outputs: one buffer per output port (edge id or -1)
  for (int i = 0; i < node->nOutputs && i < MAX_IO; i++) {
    int eid = node->outEdgeId ? node->outEdgeId[i] : -1;
    outPtrs[i] = (eid >= 0) ? lg->edges[eid].buf : lg->scratch_null;
  }

  if (node->vtable.process) {
    node->vtable.process((float *const *)inPtrs, (float *const *)outPtrs,
                         nframes, node->state);
  }
}

static void init_pending_and_seed(LiveGraph *lg) {
  int totalJobs = 0;

  // pending = indegree for reachable nodes, -1 for orphaned/deleted
  for (int i = 0; i < lg->node_count; i++) {
    // Skip deleted nodes (state == NULL)
    if (lg->nodes[i].state == NULL) {
      atomic_store_explicit(&lg->pending[i], -1, memory_order_relaxed);
      continue;
    }
    if (lg->is_orphaned[i]) {
      atomic_store_explicit(&lg->pending[i], -1, memory_order_relaxed);
      continue;
    }
    int indeg = lg->indegree[i]; // maintained incrementally at edits
    atomic_store_explicit(&lg->pending[i], indeg, memory_order_relaxed);

    // Count schedulable jobs (exclude isolated reachable nodes that neither
    // produce nor consume anything; practically rare).
    bool isolated = (indeg == 0) && !node_has_any_output_connected(lg, i);
    if (!isolated)
      totalJobs++;

    // Seed true sources (reachable, indegree==0, and at least one output conn)
    if (indeg == 0 && node_has_any_output_connected(lg, i)) {
      while (!mpmc_push(lg->readyQueue, i)) {
        __asm__ __volatile__("" ::: "memory");
      }
    }
  }

  atomic_store_explicit(&lg->jobsInFlight, totalJobs, memory_order_release);
}

void process_live_block(LiveGraph *lg, int nframes) {
  // (Queue state already empty from previous block by construction.)

  init_pending_and_seed(lg);

  // Nothing to do?
  if (atomic_load_explicit(&lg->jobsInFlight, memory_order_acquire) <= 0)
    return;

  if (g_engine.workerCount > 0) {
    // Publish session for workers
    atomic_store_explicit(&g_engine.workSession, lg, memory_order_release);

    // Audio thread also helps process
    int32_t nid;
    while (mpmc_pop(lg->readyQueue, &nid)) {
      bind_and_run_live(lg, nid, nframes);

      RTNode *node = &lg->nodes[nid];
      for (int i = 0; i < node->succCount; i++) {
        int succ = node->succ[i];
        if (succ < 0 || succ >= lg->node_count)
          continue;
        if (lg->is_orphaned[succ])
          continue;
        if (atomic_fetch_sub_explicit(&lg->pending[succ], 1,
                                      memory_order_acq_rel) == 1) {
          while (!mpmc_push(lg->readyQueue, succ)) {
            __asm__ __volatile__("" ::: "memory");
          }
        }
      }
      atomic_fetch_sub_explicit(&lg->jobsInFlight, 1, memory_order_acq_rel);
    }

    // Wait/help until all jobs done
    for (;;) {
      if (atomic_load_explicit(&lg->jobsInFlight, memory_order_acquire) == 0)
        break;

      int32_t more;
      if (mpmc_pop(lg->readyQueue, &more)) {
        bind_and_run_live(lg, more, nframes);
        RTNode *node = &lg->nodes[more];
        for (int i = 0; i < node->succCount; i++) {
          int succ = node->succ[i];
          if (succ < 0 || succ >= lg->node_count)
            continue;
          if (lg->is_orphaned[succ])
            continue;
          if (atomic_fetch_sub_explicit(&lg->pending[succ], 1,
                                        memory_order_acq_rel) == 1) {
            while (!mpmc_push(lg->readyQueue, succ)) {
              __asm__ __volatile__("" ::: "memory");
            }
          }
        }
        atomic_fetch_sub_explicit(&lg->jobsInFlight, 1, memory_order_acq_rel);
      } else {
        __asm__ __volatile__("" ::: "memory");
      }
    }

    // Clear session
    atomic_store_explicit(&g_engine.workSession, NULL, memory_order_release);
  } else {
    // Single-thread fallback
    int32_t nid;
    while (mpmc_pop(lg->readyQueue, &nid)) {
      bind_and_run_live(lg, nid, nframes);

      RTNode *node = &lg->nodes[nid];
      for (int i = 0; i < node->succCount; i++) {
        int succ = node->succ[i];
        if (succ < 0 || succ >= lg->node_count)
          continue;
        if (lg->is_orphaned[succ])
          continue;
        if (atomic_fetch_sub_explicit(&lg->pending[succ], 1,
                                      memory_order_acq_rel) == 1) {
          while (!mpmc_push(lg->readyQueue, succ)) {
            __asm__ __volatile__("" ::: "memory");
          }
        }
      }
      atomic_fetch_sub_explicit(&lg->jobsInFlight, 1, memory_order_acq_rel);
    }
  }
}

int find_live_output(LiveGraph *lg) {
  return lg->dac_node_id; // Simply return the DAC node - no searching needed
}

// ===================== Live Engine Implementation =====================
void process_next_block(LiveGraph *lg, float *output_buffer, int nframes) {
  if (!lg || !output_buffer || nframes <= 0) {
    // Clear output buffer if invalid input
    if (output_buffer && nframes > 0) {
      memset(output_buffer, 0, nframes * sizeof(float));
    }
    return;
  }

  apply_graph_edits(lg->graphEditQueue, lg);
  apply_params(lg);
  process_live_block(lg, nframes);
  int output_node = find_live_output(lg);
  if (output_node >= 0 && lg->nodes[output_node].nInputs > 0) {
    int master_edge_id = lg->nodes[output_node].inEdgeId[0];
    if (master_edge_id >= 0 && master_edge_id < lg->edge_capacity) {
      memcpy(output_buffer, lg->edges[master_edge_id].buf,
             nframes * sizeof(float));
    }
  } else {
    // handle case where theres no output node (silence)
    memset(output_buffer, 0, nframes * sizeof(float));
  }
}

// ===================== Failed ID Tracking =====================

void add_failed_id(LiveGraph *lg, uint64_t logical_id) {
  // Expand capacity if needed
  if (lg->failed_ids_count >= lg->failed_ids_capacity) {
    lg->failed_ids_capacity *= 2;
    lg->failed_ids = realloc(lg->failed_ids, lg->failed_ids_capacity * sizeof(uint64_t));
  }
  lg->failed_ids[lg->failed_ids_count++] = logical_id;
}

bool is_failed_node(LiveGraph *lg, int logical_id) {
  // Check if this logical ID is in the failed list
  for (int i = 0; i < lg->failed_ids_count; i++) {
    if (lg->failed_ids[i] == (uint64_t)logical_id) {
      return true;
    }
  }
  return false;
}

// ===================== Queue-based API =====================

int add_node(LiveGraph *lg, NodeVTable vtable, void *state, const char *name, int nInputs, int nOutputs) {
  // Atomically allocate the next node ID (which is also the array index)
  int node_id = atomic_fetch_add(&lg->next_node_id, 1);
  
  // Create the command
  GraphEditCmd cmd = {
    .op = GE_ADD_NODE,
    .u.add_node = {
      .vt = vtable,
      .state = state,
      .logical_id = node_id,  // Use node_id as the logical_id (they're the same)
      .name = (char *)name,
      .nInputs = nInputs,
      .nOutputs = nOutputs
    }
  };
  
  // Queue the command
  if (!geq_push(lg->graphEditQueue, &cmd)) {
    // Queue full - consider this a failure
    add_failed_id(lg, node_id);
    return -1;
  }
  
  // Return the pre-allocated node ID (which is both logical_id and array index)
  return node_id;
}

bool delete_node(LiveGraph *lg, int node_id) {
  GraphEditCmd cmd = {
    .op = GE_REMOVE_NODE,
    .u.remove_node = {
      .node_id = node_id
    }
  };
  
  return geq_push(lg->graphEditQueue, &cmd);
}

bool connect(LiveGraph *lg, int src_node, int src_port, int dst_node, int dst_port) {
  // Check if either node has failed
  if (is_failed_node(lg, src_node) || is_failed_node(lg, dst_node)) {
    return false;
  }
  
  GraphEditCmd cmd = {
    .op = GE_CONNECT,
    .u.connect = {
      .src_id = src_node,
      .src_port = src_port,
      .dst_id = dst_node,
      .dst_port = dst_port
    }
  };
  
  return geq_push(lg->graphEditQueue, &cmd);
}

bool disconnect(LiveGraph *lg, int src_node, int src_port, int dst_node, int dst_port) {
  GraphEditCmd cmd = {
    .op = GE_DISCONNECT,
    .u.disconnect = {
      .src_id = src_node,
      .src_port = src_port,
      .dst_id = dst_node,
      .dst_port = dst_port
    }
  };
  
  return geq_push(lg->graphEditQueue, &cmd);
}
