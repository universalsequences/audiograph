#include "graph_engine.h"
#include "graph_nodes.h"

// ===================== Forward Declarations =====================

void bind_and_run_live(LiveGraph *lg, int nid, int nframes);

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

void bind_and_run(GraphState *g, int nid, int nframes) {
  RTNode *node = &g->nodes[nid];

  // Debug output: Show which thread is processing which node
  pthread_t thread_id = pthread_self();
  printf("    [WORKER %lu] Processing node_id=%d (logical_id=%llu) for %d "
         "samples\n",
         (unsigned long)thread_id, nid, node->logical_id, nframes);

  // RACE DETECTION: Check if this node is already being processed
  static _Atomic int processing_nodes[64] = {0}; // Assumes max 64 nodes
  int expected = 0;
  if (!atomic_compare_exchange_strong(&processing_nodes[nid], &expected, 1)) {
    printf("    [RACE DETECTED] Node %d already being processed by another "
           "worker!\n",
           nid);
  }

  printf("    [DEBUG] node_id=%d has %d inputs, %d outputs\n", nid,
         node->nInputs, node->nOutputs);

  // Skip processing nodes with no outputs - they're just master edge markers
  if (node->nOutputs == 0) {
    printf("    [WORKER %lu] Skipping node_id=%d (no outputs - master edge "
           "marker)\n",
           (unsigned long)thread_id, nid);
    // RACE DETECTION: Reset processing flag
    atomic_store(&processing_nodes[nid], 0);
    return;
  }

  float *inPtrsStatic[MAX_IO];
  float *outPtrsStatic[MAX_IO];
  if (g->edgeBufs == NULL) {
    printf("    [ERROR] g->edgeBufs is NULL!\n");
    return;
  }

  for (int i = 0; i < node->nInputs; i++) {
    int edge_idx = node->inEdges[i];
    printf("    [DEBUG] Input %d: edge_idx=%d\n", i, edge_idx);
    if (edge_idx < 0 || edge_idx >= g->edgeCount) {
      printf("    [ERROR] Invalid input edge index %d (edgeCount=%d)\n",
             edge_idx, g->edgeCount);
      return;
    }
    if (g->edgeBufs[edge_idx] == NULL) {
      printf("    [ERROR] Input edge buffer %d is NULL\n", edge_idx);
      return;
    }
    inPtrsStatic[i] = g->edgeBufs[edge_idx];
  }

  for (int i = 0; i < node->nOutputs; i++) {
    int edge_idx = node->outEdges[i];
    printf("    [DEBUG] Output %d: edge_idx=%d\n", i, edge_idx);
    if (edge_idx < 0 || edge_idx >= g->edgeCount) {
      printf("    [ERROR] Invalid output edge index %d (edgeCount=%d)\n",
             edge_idx, g->edgeCount);
      return;
    }
    if (g->edgeBufs[edge_idx] == NULL) {
      printf("    [ERROR] Output edge buffer %d is NULL\n", edge_idx);
      return;
    }
    outPtrsStatic[i] = g->edgeBufs[edge_idx];
  }
  // DEBUG: Verify state pointer integrity
  printf("    [STATE_DEBUG] node_id=%d state_ptr=%p\n", nid,
         (void *)node->state);
  if (node->state != NULL && node->vtable.process == osc_process) {
    float *mem = (float *)node->state;
    printf("    [STATE_DEBUG] Before process: phase=%.6f, inc=%.6f\n", mem[0],
           mem[1]);
  }

  node->vtable.process((float *const *)inPtrsStatic,
                       (float *const *)outPtrsStatic, nframes, node->state);

  // DEBUG: Check state after processing
  if (node->state != NULL && node->vtable.process == osc_process) {
    float *mem = (float *)node->state;
    printf("    [STATE_DEBUG] After process: phase=%.6f, inc=%.6f\n", mem[0],
           mem[1]);
  }

  // RACE DETECTION: Reset processing flag
  atomic_store(&processing_nodes[nid], 0);

  // DEBUG: Show first few samples of each output buffer
  for (int i = 0; i < node->nOutputs; i++) {
    printf("    [OUTPUT] node_id=%d output[%d]: [%.6f, %.6f, %.6f, %.6f]\n",
           nid, i, outPtrsStatic[i][0], outPtrsStatic[i][1],
           outPtrsStatic[i][2], outPtrsStatic[i][3]);
  }

  printf("    [WORKER %lu] Completed node_id=%d (logical_id=%llu)\n",
         (unsigned long)thread_id, nid, node->logical_id);
}

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

LiveGraph *create_live_graph(int initial_capacity, int block_size,
                             const char *label) {
  LiveGraph *lg = calloc(1, sizeof(LiveGraph));

  // Node storage
  lg->node_capacity = initial_capacity;
  lg->nodes = calloc(lg->node_capacity, sizeof(RTNode));
  lg->pending = calloc(lg->node_capacity, sizeof(atomic_int));
  lg->is_orphaned = calloc(lg->node_capacity, sizeof(bool));

  // Edge pool (start with generous capacity)
  lg->edge_capacity = initial_capacity * 4;
  lg->edge_buffers = calloc(lg->edge_capacity, sizeof(float *));
  lg->edge_free = calloc(lg->edge_capacity, sizeof(bool));
  lg->block_size = block_size;

  // Pre-allocate all edge buffers
  for (int i = 0; i < lg->edge_capacity; i++) {
    lg->edge_buffers[i] = alloc_aligned(64, block_size * sizeof(float));
    lg->edge_free[i] = true; // all edges start free
  }

  // Connection tracking
  lg->connection_capacity = initial_capacity * 8;
  lg->connections = calloc(lg->connection_capacity, sizeof(LiveConnection));

  // Ready queue (MPMC for thread safety)
  lg->readyQueue = mpmc_create(1024);
  if (!lg->readyQueue) {
    // Handle allocation failure
    for (int i = 0; i < lg->edge_capacity; i++) {
      free(lg->edge_buffers[i]);
    }
    free(lg->edge_buffers);
    free(lg->edge_free);
    free(lg->connections);
    free(lg->nodes);
    free(lg->pending);
    free(lg->is_orphaned);
    free(lg);
    return NULL;
  }

  // Parameter mailbox
  lg->params = calloc(1, sizeof(ParamRing));

  // Initialize DAC node (no DAC connected initially)
  lg->dac_node_id = -1;

  lg->label = label;
  return lg;
}

static int find_free_edge(LiveGraph *lg) {
  for (int i = 0; i < lg->edge_capacity; i++) {
    if (lg->edge_free[i]) {
      lg->edge_free[i] = false;
      return i;
    }
  }
  return -1; // no free edges (should expand pool)
}

static void free_edge(LiveGraph *lg, int edge_id) {
  if (edge_id >= 0 && edge_id < lg->edge_capacity) {
    lg->edge_free[edge_id] = true;
    // Clear the buffer
    memset(lg->edge_buffers[edge_id], 0, lg->block_size * sizeof(float));
  }
}

int live_add_node(LiveGraph *lg, NodeVTable vtable, void *state,
                  uint64_t logical_id, const char *name) {
  // Find free slot or expand
  int node_id = lg->node_count;
  if (node_id >= lg->node_capacity) {
    // Need to expand - for demo just fail
    return -1;
  }

  RTNode *node = &lg->nodes[node_id];
  memset(node, 0, sizeof(RTNode));

  node->logical_id = logical_id;
  node->vtable = vtable;
  node->state = state;
  node->nInputs = 0;
  node->nOutputs = 0;
  node->faninBase = 0;
  node->succCount = 0;

  // Initialize as empty (no connections yet)
  node->inEdges = NULL;
  node->outEdges = NULL;
  node->succ = NULL;

  // Initialize orphaned state - new nodes with no connections start as orphaned
  // They will be marked as non-orphaned when they get connected to the signal
  // path
  lg->is_orphaned[node_id] = true;

  lg->node_count++;

  return node_id;
}

int live_add_oscillator(LiveGraph *lg, float freq_hz, const char *name) {
  float *memory = calloc(OSC_MEMORY_SIZE, sizeof(float));
  memory[OSC_PHASE] = 0.0f;
  memory[OSC_INC] = freq_hz / 48000.0f;
  return live_add_node(lg, OSC_VTABLE, memory, ++g_next_node_id, name);
}

int live_add_gain(LiveGraph *lg, float gain_value, const char *name) {
  float *memory = calloc(GAIN_MEMORY_SIZE, sizeof(float));
  memory[GAIN_VALUE] = gain_value;
  return live_add_node(lg, GAIN_VTABLE, memory, ++g_next_node_id, name);
}

int live_add_mixer2(LiveGraph *lg, const char *name) {
  return live_add_node(lg, MIX2_VTABLE, NULL, ++g_next_node_id, name);
}

int live_add_mixer8(LiveGraph *lg, const char *name) {
  return live_add_node(lg, MIX8_VTABLE, NULL, ++g_next_node_id, name);
}

// DAC function moved after helper function declarations

// Helper functions for live connections
static void add_input_edge(RTNode *node, int edge_id) {
  // Expand input array
  node->inEdges = realloc(node->inEdges, (node->nInputs + 1) * sizeof(int32_t));
  node->inEdges[node->nInputs] = edge_id;
  node->nInputs++;
  node->faninBase++; // more dependencies
}

static void add_output_edge(RTNode *node, int edge_id) {
  // Expand output array
  node->outEdges =
      realloc(node->outEdges, (node->nOutputs + 1) * sizeof(int32_t));
  node->outEdges[node->nOutputs] = edge_id;
  node->nOutputs++;
}

static void add_successor(RTNode *node, int succ_id) {
  // Expand successor array
  node->succ = realloc(node->succ, (node->succCount + 1) * sizeof(int32_t));
  node->succ[node->succCount] = succ_id;
  node->succCount++;
}

// Recursive function to mark nodes reachable from DAC
static void mark_reachable_from_dac(LiveGraph *lg, int node_id, bool *visited) {
  if (node_id < 0 || node_id >= lg->node_count || visited[node_id]) {
    return;
  }

  visited[node_id] = true;
  lg->is_orphaned[node_id] = false; // Mark as not orphaned

  RTNode *node = &lg->nodes[node_id];
  // Traverse backwards through input edges to find all nodes that feed this one
  for (int i = 0; i < node->nInputs; i++) {
    int edge_id = node->inEdges[i];
    // Find the source node that outputs to this edge
    for (int j = 0; j < lg->node_count; j++) {
      RTNode *potential_source = &lg->nodes[j];
      for (int k = 0; k < potential_source->nOutputs; k++) {
        if (potential_source->outEdges[k] == edge_id) {
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

bool live_connect(LiveGraph *lg, int source_id, int dest_id) {
  if (source_id < 0 || source_id >= lg->node_count || dest_id < 0 ||
      dest_id >= lg->node_count) {
    return false;
  }

  // Get a free edge buffer
  int edge_id = find_free_edge(lg);
  if (edge_id < 0) {
    return false;
  }

  RTNode *source = &lg->nodes[source_id];
  RTNode *dest = &lg->nodes[dest_id];

  // Add edge to source's outputs
  add_output_edge(source, edge_id);

  // Add edge to dest's inputs
  add_input_edge(dest, edge_id);

  // Add dest to source's successors
  add_successor(source, dest_id);

  // Update orphaned status based on DAC reachability
  update_orphaned_status(lg);

  // Record the connection
  if (lg->connection_count < lg->connection_capacity) {
    LiveConnection *conn = &lg->connections[lg->connection_count++];
    conn->source_node_id = source_id;
    conn->dest_node_id = dest_id;
    conn->edge_buffer_id = edge_id;
    conn->active = true;
  }

  return true;
}

int live_add_dac(LiveGraph *lg, const char *name) {
  int dac_id = live_add_node(lg, DAC_VTABLE, NULL, ++g_next_node_id, name);
  if (dac_id >= 0) {
    lg->dac_node_id = dac_id; // Remember the DAC node

    // Allocate an output edge for the DAC so we can read the final audio
    int output_edge = find_free_edge(lg);
    if (output_edge >= 0) {
      RTNode *dac = &lg->nodes[dac_id];
      add_output_edge(dac, output_edge);
    }
  }
  return dac_id;
}

bool live_disconnect(LiveGraph *lg, int source_id, int dest_id) {
  if (source_id < 0 || source_id >= lg->node_count || dest_id < 0 ||
      dest_id >= lg->node_count) {
    return false;
  }

  // Find the connection
  LiveConnection *conn = NULL;
  for (int i = 0; i < lg->connection_count; i++) {
    if (lg->connections[i].source_node_id == source_id &&
        lg->connections[i].dest_node_id == dest_id &&
        lg->connections[i].active) {
      conn = &lg->connections[i];
      break;
    }
  }

  if (!conn) {
    return false;
  }

  RTNode *source = &lg->nodes[source_id];
  RTNode *dest = &lg->nodes[dest_id];
  int edge_id = conn->edge_buffer_id;

  // Remove edge from source outputs (find and swap with last)
  bool found_output = false;
  if (source->outEdges && source->nOutputs > 0) {
    for (int i = 0; i < source->nOutputs; i++) {
      if (source->outEdges[i] == edge_id) {
        // Swap with last element
        if (source->nOutputs > 1) {
          source->outEdges[i] = source->outEdges[source->nOutputs - 1];
        }
        source->nOutputs--;
        found_output = true;
        break;
      }
    }
  }

  // Remove edge from dest inputs
  bool found_input = false;
  if (dest->inEdges && dest->nInputs > 0) {
    for (int i = 0; i < dest->nInputs; i++) {
      if (dest->inEdges[i] == edge_id) {
        // Swap with last element
        if (dest->nInputs > 1) {
          dest->inEdges[i] = dest->inEdges[dest->nInputs - 1];
        }
        dest->nInputs--;
        dest->faninBase--; // fewer dependencies
        found_input = true;
        break;
      }
    }
  }

  // Remove from source successors
  bool found_succ = false;
  if (source->succ && source->succCount > 0) {
    for (int i = 0; i < source->succCount; i++) {
      if (source->succ[i] == dest_id) {
        // Swap with last element
        if (source->succCount > 1) {
          source->succ[i] = source->succ[source->succCount - 1];
        }
        source->succCount--;
        found_succ = true;
        break;
      }
    }
  }

  if (!found_output || !found_input || !found_succ) {
    printf("  WARNING: Inconsistent connection state during disconnect\n");
  }

  // Update orphaned status based on DAC reachability
  update_orphaned_status(lg);

  // Free the edge buffer
  free_edge(lg, edge_id);

  // Mark connection as inactive
  conn->active = false;

  printf("Disconnected node %d -> node %d (freed edge %d)\n", source_id,
         dest_id, edge_id);
  return true;
}

void bind_and_run_live(LiveGraph *lg, int nid, int nframes) {
  if (nid < 0 || nid >= lg->node_count) {
    printf("ERROR: Invalid node ID %d\n", nid);
    return;
  }

  RTNode *node = &lg->nodes[nid];

  // Validate node data structures
  if (node->nInputs < 0 || node->nOutputs < 0 || node->nInputs > MAX_IO ||
      node->nOutputs > MAX_IO) {
    printf("ERROR: Invalid node %d I/O counts: in=%d out=%d\n", nid,
           node->nInputs, node->nOutputs);
    return;
  }

  // Skip orphaned nodes entirely - they shouldn't be in the scheduling queue
  // anyway
  if (lg->is_orphaned[nid]) {
    return;
  }

  // Don't process if arrays are NULL (should not happen but safety check)
  if ((node->nInputs > 0 && !node->inEdges) ||
      (node->nOutputs > 0 && !node->outEdges)) {
    return;
  }

  float *inPtrsStatic[MAX_IO];
  float *outPtrsStatic[MAX_IO];

  // Bounds check and populate input pointers
  for (int i = 0; i < node->nInputs && i < MAX_IO; i++) {
    int edge_id = node->inEdges[i];
    if (edge_id >= 0 && edge_id < lg->edge_capacity) {
      inPtrsStatic[i] = lg->edge_buffers[edge_id];
    } else {
      inPtrsStatic[i] = lg->edge_buffers[0]; // fallback to first buffer
    }
  }

  // Bounds check and populate output pointers
  for (int i = 0; i < node->nOutputs && i < MAX_IO; i++) {
    int edge_id = node->outEdges[i];
    if (edge_id >= 0 && edge_id < lg->edge_capacity) {
      outPtrsStatic[i] = lg->edge_buffers[edge_id];
    } else {
      outPtrsStatic[i] = lg->edge_buffers[0]; // fallback to first buffer
    }
  }

  if (node->vtable.process) {
    node->vtable.process((float *const *)inPtrsStatic,
                         (float *const *)outPtrsStatic, nframes, node->state);
  }
}

void process_live_block(LiveGraph *lg, int nframes) {
  // Reset scheduling state (MPMC queue doesn't need manual reset)
  for (int i = 0; i < lg->node_count; i++) {
    if (lg->is_orphaned[i]) {
      // Orphaned nodes don't participate in scheduling
      atomic_store_explicit(&lg->pending[i], -1, memory_order_relaxed);
    } else {
      // Calculate actual dependencies excluding orphaned predecessors
      int actual_deps = 0;
      for (int j = 0; j < lg->nodes[i].nInputs; j++) {
        // Find which node produces this input edge
        int input_edge = lg->nodes[i].inEdges[j];
        bool found_producer = false;
        for (int k = 0; k < lg->node_count; k++) {
          if (lg->is_orphaned[k])
            continue; // skip orphaned nodes
          for (int m = 0; m < lg->nodes[k].nOutputs; m++) {
            if (lg->nodes[k].outEdges[m] == input_edge) {
              actual_deps++;
              found_producer = true;
              break;
            }
          }
          if (found_producer)
            break;
        }
      }
      atomic_store_explicit(&lg->pending[i], actual_deps, memory_order_relaxed);
    }
  }

  // Seed source nodes (nodes with no inputs AND have outputs, skip orphaned and
  // isolated nodes)
  int totalJobs = 0;
  int sources_found = 0;
  for (int i = 0; i < lg->node_count; i++) {
    RTNode *node = &lg->nodes[i];

    // A true source has no inputs but DOES have outputs
    bool is_true_source = (atomic_load(&lg->pending[i]) == 0) &&
                          (node->nOutputs > 0) && !lg->is_orphaned[i];

    // Count jobs: exclude orphaned nodes and isolated nodes (no inputs, no
    // outputs)
    bool is_isolated = (node->nInputs == 0) && (node->nOutputs == 0);
    bool should_process = !lg->is_orphaned[i] && !is_isolated;

    if (is_true_source) {
      while (!mpmc_push(lg->readyQueue, i)) { /* spin */
        __asm__ __volatile__("" ::: "memory");
      }
      sources_found++;
    }

    if (should_process) {
      totalJobs++;
    }
  }
  atomic_store_explicit(&lg->jobsInFlight, totalJobs, memory_order_release);

  if (sources_found == 0) {
    return;
  }

  // Process nodes with multi-threaded workers using MPMC queue
  if (g_engine.workerCount > 0) {
    // Multi-threaded path: use worker threads with MPMC queue
    atomic_store_explicit(&g_engine.workSession, lg, memory_order_release);

    // Process some jobs on audio thread while workers help
    int32_t nid;
    while (mpmc_pop(lg->readyQueue, &nid)) {
      bind_and_run_live(lg, nid, nframes);
      RTNode *node = &lg->nodes[nid];

      // Notify successors
      if (node->succ && node->succCount > 0) {
        for (int i = 0; i < node->succCount; i++) {
          int succ = node->succ[i];
          if (succ >= 0 && succ < lg->node_count && !lg->is_orphaned[succ]) {
            if (atomic_fetch_sub_explicit(&lg->pending[succ], 1,
                                          memory_order_acq_rel) == 1) {
              while (!mpmc_push(lg->readyQueue, succ)) {
                // Queue full, yield briefly
                __asm__ __volatile__("" ::: "memory");
              }
            }
          }
        }
      }
      atomic_fetch_sub_explicit(&lg->jobsInFlight, 1, memory_order_acq_rel);
    }

    // Wait for all jobs to complete
    while (atomic_load_explicit(&lg->jobsInFlight, memory_order_acquire) > 0) {
      // Keep trying to help with remaining work
      if (mpmc_pop(lg->readyQueue, &nid)) {
        bind_and_run_live(lg, nid, nframes);
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
      } else {
        // No work available, yield CPU briefly
        __asm__ __volatile__("" ::: "memory");
      }
    }

    atomic_store_explicit(&g_engine.workSession, NULL, memory_order_release);
  } else {
    // Single-threaded fallback path
    int32_t nid;
    while (mpmc_pop(lg->readyQueue, &nid)) {
      bind_and_run_live(lg, nid, nframes);
      RTNode *node = &lg->nodes[nid];

      // Notify successors
      if (node->succ && node->succCount > 0) {
        for (int i = 0; i < node->succCount; i++) {
          int succ = node->succ[i];
          if (succ >= 0 && succ < lg->node_count && !lg->is_orphaned[succ]) {
            if (atomic_fetch_sub_explicit(&lg->pending[succ], 1,
                                          memory_order_acq_rel) == 1) {
              while (!mpmc_push(lg->readyQueue, succ)) { /* spin */
                __asm__ __volatile__("" ::: "memory");
              }
            }
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

  process_live_block(lg, nframes);
  int output_node = find_live_output(lg);
  if (output_node >= 0 && lg->nodes[output_node].nInputs > 0) {
    int master_edge = lg->nodes[output_node].inEdges[0];
    memcpy(output_buffer, lg->edge_buffers[master_edge],
           nframes * sizeof(float));
  } else {
    // handle case where theres no output node (silence)
    memset(output_buffer, 0, nframes * sizeof(float));
  }
}
