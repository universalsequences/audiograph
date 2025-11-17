#include "graph_api.h"
#include "graph_edit.h"
#include "graph_nodes.h"

LiveGraph *create_live_graph(int initial_capacity, int block_size,
                             const char *label, int num_channels) {
  LiveGraph *lg = calloc(1, sizeof(LiveGraph));

  // Node storage
  lg->node_capacity = initial_capacity;
  lg->nodes = calloc(lg->node_capacity, sizeof(RTNode));
  lg->pending = calloc(lg->node_capacity, sizeof(atomic_int));
  lg->indegree = calloc(lg->node_capacity, sizeof(int));
  lg->is_orphaned = calloc(lg->node_capacity, sizeof(bool));

  // Edge pool (start with generous capacity)
  lg->edge_capacity = initial_capacity * 32;
  lg->block_size = block_size;
  lg->num_channels = (num_channels > 0) ? num_channels : 1; // Default to mono

  // Initialize Retire List
  lg->retire_capacity = 32;
  lg->retire_count = 0;
  lg->retire_list = calloc(lg->retire_capacity, sizeof(RetireEntry));

  // New port-based edge pool
  lg->edges = calloc(lg->edge_capacity, sizeof(LiveEdge));
  for (int i = 0; i < lg->edge_capacity; i++) {
    lg->edges[i].buf = alloc_aligned(64, block_size * sizeof(float));
    if (!lg->edges[i].buf) {
      return NULL;
    }
    lg->edges[i].in_use = false;
    lg->edges[i].refcount = 0;
    lg->edges[i].src_node = -1;
    lg->edges[i].src_port = -1;
  }

  // Support buffers for port system
  lg->silence_buf = alloc_aligned(64, block_size * sizeof(float));
  lg->scratch_null = alloc_aligned(64, block_size * sizeof(float));
  memset(lg->silence_buf, 0,
         block_size * sizeof(float)); // keep silence buffer zeroed

  // Ready queue (ReadyQ with MPMC + semaphore for thread safety)
  // BURST FIX: Increased capacity from 1024 to 4096 for wide graphs
  lg->readyQueue = rq_create(4096);
  if (!lg->readyQueue) {
    // Handle allocation failure - clean up port-based edges
    for (int i = 0; i < lg->edge_capacity; i++) {
      free(lg->edges[i].buf);
    }
    free(lg->edges);
    free(lg->silence_buf);
    free(lg->scratch_null);
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
  geq_init(lg->graphEditQueue, 8192);

  // Initialize failed IDs tracking
  lg->failed_ids_capacity = 64; // Start with reasonable capacity
  lg->failed_ids = calloc(lg->failed_ids_capacity, sizeof(uint64_t));
  lg->failed_ids_count = 0;

  // Initialize atomic node ID counter (start at 1 to avoid confusion with DAC
  // at 0)
  atomic_init(&lg->next_node_id, 1);

  // Initialize watch list system
  lg->watch_list_capacity = 16;
  lg->watch_list = calloc(lg->watch_list_capacity, sizeof(int));
  lg->watch_list_count = 0;
  pthread_mutex_init(&lg->watch_list_mutex, NULL);

  // Initialize state store arrays (indexed by node_id)
  lg->state_snapshots = calloc(lg->node_capacity, sizeof(void *));
  lg->state_sizes = calloc(lg->node_capacity, sizeof(size_t));
  pthread_rwlock_init(&lg->state_store_lock, NULL);

  // Automatically create the DAC node at index 0
  // DAC has one input and one output per channel
  int dac_id = apply_add_node(lg, DAC_VTABLE, 0, 0, "DAC", lg->num_channels,
                              lg->num_channels, NULL);
  if (dac_id >= 0) {
    lg->dac_node_id = dac_id; // Remember the DAC node

    RTNode *dac = &lg->nodes[dac_id];
    dac->nInputs = lg->num_channels; // DAC has N inputs (one per channel)
    dac->nOutputs =
        lg->num_channels; // DAC has N outputs (for reading final audio)
    if (!ensure_port_arrays(dac)) {
      // DAC node allocation failed - this is critical
      return NULL;
    }

    // Allocate output edges for each channel of the DAC
    for (int ch = 0; ch < lg->num_channels; ch++) {
      int output_edge = alloc_edge(lg);
      if (output_edge >= 0) {
        dac->outEdgeId[ch] = output_edge; // Use port-based system
      }
    }
  }

  lg->label = label;
  return lg;
}

void destroy_live_graph(LiveGraph *lg) {
  if (!lg)
    return;

  // Free all edge buffers
  if (lg->edges) {
    for (int i = 0; i < lg->edge_capacity; i++) {
      if (lg->edges[i].buf) {
        free(lg->edges[i].buf);
      }
    }
    free(lg->edges);
  }

  // Free all node state and port arrays
  if (lg->nodes) {
    for (int i = 0; i < lg->node_count; i++) { // Use count, not capacity
      RTNode *node = &lg->nodes[i];

      if (node->state) {
        free(node->state);
      }
      if (node->inEdgeId) {
        free(node->inEdgeId);
      }
      if (node->outEdgeId) {
        free(node->outEdgeId);
      }
      if (node->fanin_sum_node_id) {
        free(node->fanin_sum_node_id);
      }
      if (node->succ) {
        free(node->succ);
      }
    }
    free(lg->nodes);
  }

  // Free scheduling arrays
  if (lg->pending)
    free(lg->pending);
  if (lg->indegree)
    free(lg->indegree);
  if (lg->is_orphaned)
    free(lg->is_orphaned);

  // Free support buffers
  if (lg->silence_buf)
    free(lg->silence_buf);
  if (lg->scratch_null)
    free(lg->scratch_null);

  // Free queues
  if (lg->readyQueue)
    rq_destroy(lg->readyQueue);
  if (lg->params)
    free(lg->params);
  if (lg->graphEditQueue) {
    if (lg->graphEditQueue->buf)
      free(lg->graphEditQueue->buf);
    free(lg->graphEditQueue);
  }

  // Free failed IDs tracking
  if (lg->failed_ids)
    free(lg->failed_ids);

  // Free watch list system
  if (lg->watch_list)
    free(lg->watch_list);
  pthread_mutex_destroy(&lg->watch_list_mutex);

  // Free state snapshots
  if (lg->state_snapshots) {
    for (int i = 0; i < lg->node_capacity; i++) {
      if (lg->state_snapshots[i]) {
        free(lg->state_snapshots[i]);
      }
    }
    free(lg->state_snapshots);
  }
  if (lg->state_sizes)
    free(lg->state_sizes);
  pthread_rwlock_destroy(&lg->state_store_lock);

  // Free the graph itself
  free(lg);
}

static int allocate_logical_id(LiveGraph *lg) {
  return atomic_fetch_add(&lg->next_node_id, 1);
}

static int finalize_live_add(LiveGraph *lg, int node_id, NodeVTable vtable,
                             size_t state_size, const char *name, int nInputs,
                             int nOutputs, const void *initial_state) {
  int result = apply_add_node(lg, vtable, state_size, (uint64_t)node_id, name,
                              nInputs, nOutputs, initial_state);
  if (result < 0) {
    add_failed_id(lg, node_id);
    return -1;
  }
  return result;
}

int live_add_oscillator(LiveGraph *lg, float freq_hz, const char *name) {
  if (!lg)
    return -1;
  int node_id = allocate_logical_id(lg);
  float init_state[1];
  if (freq_hz < 0.0f)
    freq_hz = 0.0f;
  init_state[0] = freq_hz / 48000.0f; // default sample rate assumption
  return finalize_live_add(lg, node_id, OSC_VTABLE,
                           OSC_MEMORY_SIZE * sizeof(float), name, 0, 1,
                           init_state);
}

int live_add_gain(LiveGraph *lg, float gain_value, const char *name) {
  if (!lg)
    return -1;
  int node_id = allocate_logical_id(lg);
  float init_state[1];
  init_state[0] = gain_value;
  return finalize_live_add(lg, node_id, GAIN_VTABLE,
                           GAIN_MEMORY_SIZE * sizeof(float), name, 2, 1,
                           init_state);
}

int live_add_number(LiveGraph *lg, float value, const char *name) {
  if (!lg)
    return -1;
  int node_id = allocate_logical_id(lg);
  float init_state[1];
  init_state[0] = value;
  return finalize_live_add(lg, node_id, NUMBER_VTABLE,
                           NUMBER_MEMORY_SIZE * sizeof(float), name, 0, 1,
                           init_state);
}

int live_add_mixer2(LiveGraph *lg, const char *name) {
  if (!lg)
    return -1;
  int node_id = allocate_logical_id(lg);
  return finalize_live_add(lg, node_id, MIX2_VTABLE, 0, name, 2, 1, NULL);
}

int live_add_mixer8(LiveGraph *lg, const char *name) {
  if (!lg)
    return -1;
  int node_id = allocate_logical_id(lg);
  return finalize_live_add(lg, node_id, MIX8_VTABLE, 0, name, 8, 1, NULL);
}

int live_add_sum(LiveGraph *lg, const char *name, int nInputs) {
  if (!lg || nInputs <= 0)
    return -1;
  int node_id = allocate_logical_id(lg);
  return finalize_live_add(lg, node_id, SUM_VTABLE, 0, name, nInputs, 1, NULL);
}

int add_node(LiveGraph *lg, NodeVTable vtable, size_t state_size,
             const char *name, int nInputs, int nOutputs,
             const void *initial_state, size_t initial_state_size) {
  // Atomically allocate the next node ID (which is also the array index)
  int node_id = atomic_fetch_add(&lg->next_node_id, 1);

  // Create a copy of initial_state if provided
  void *initial_state_copy = NULL;
  if (initial_state && initial_state_size > 0) {
    initial_state_copy = malloc(initial_state_size);
    if (initial_state_copy) {
      memcpy(initial_state_copy, initial_state, initial_state_size);
    }
  }

  // Create the command
  GraphEditCmd cmd = {
      .op = GE_ADD_NODE,
      .u.add_node = {
          .vt = vtable,
          .state_size = state_size,
          .logical_id =
              node_id, // Use node_id as the logical_id (they're the same)
          .name = (char *)name,
          .nInputs = nInputs,
          .nOutputs = nOutputs,
          .initial_state = initial_state_copy,
          .initial_state_size = initial_state_size}};

  // Queue the command
  if (!geq_push(lg->graphEditQueue, &cmd)) {
    // Queue full - consider this a failure
    if (initial_state_copy) {
      free(initial_state_copy);
    }
    add_failed_id(lg, node_id);
    return -1;
  }

  // Return the pre-allocated node ID (which is both logical_id and array index)
  return node_id;
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

bool delete_node(LiveGraph *lg, int node_id) {
  GraphEditCmd cmd = {.op = GE_REMOVE_NODE,
                      .u.remove_node = {.node_id = node_id}};

  return geq_push(lg->graphEditQueue, &cmd);
}

bool graph_connect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                   int dst_port) {
  // Check if either node has failed
  if (is_failed_node(lg, src_node) || is_failed_node(lg, dst_node)) {
    printf("could not connect because failed node\n");
    return false;
  }

  GraphEditCmd cmd = {.op = GE_CONNECT,
                      .u.connect = {.src_id = src_node,
                                    .src_port = src_port,
                                    .dst_id = dst_node,
                                    .dst_port = dst_port}};

  return geq_push(lg->graphEditQueue, &cmd);
}

bool graph_disconnect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                      int dst_port) {
  GraphEditCmd cmd = {.op = GE_DISCONNECT,
                      .u.disconnect = {.src_id = src_node,
                                       .src_port = src_port,
                                       .dst_id = dst_node,
                                       .dst_port = dst_port}};

  return geq_push(lg->graphEditQueue, &cmd);
}

bool hot_swap_node(LiveGraph *lg, int node_id, NodeVTable vt, size_t state_size,
                   int nin, int nout, bool xfade,
                   void (*migrate)(void *, void *), const void *initial_state,
                   size_t initial_state_size) {
  if (is_failed_node(lg, node_id)) {
    return false;
  }

  // Create a copy of initial_state if provided
  void *initial_state_copy = NULL;
  if (initial_state && initial_state_size > 0) {
    initial_state_copy = malloc(initial_state_size);
    if (initial_state_copy) {
      memcpy(initial_state_copy, initial_state, initial_state_size);
    }
  }

  GraphEditCmd cmd = {.op = GE_HOT_SWAP_NODE,
                      .u.hot_swap_node =
                          {
                              .vt = vt,
                              .state_size = state_size,
                              .node_id = node_id,
                              .new_nInputs = nin,
                              .new_nOutputs = nout,
                              .initial_state = initial_state_copy,
                              .initial_state_size = initial_state_size,
                          }

  };
  if (!geq_push(lg->graphEditQueue, &cmd)) {
    // Queue push failed - clean up copied memory
    if (initial_state_copy) {
      free(initial_state_copy);
    }
    return false;
  }
  return true;
}

bool replace_keep_edges(LiveGraph *lg, int node_id, NodeVTable vt,
                        size_t state_size, int nin, int nout, bool xfade,
                        void (*migrate)(void *, void *),
                        const void *initial_state, size_t initial_state_size) {
  // Create a copy of initial_state if provided
  void *initial_state_copy = NULL;
  if (initial_state && initial_state_size > 0) {
    initial_state_copy = malloc(initial_state_size);
    if (initial_state_copy) {
      memcpy(initial_state_copy, initial_state, initial_state_size);
    }
  }

  GraphEditCmd cmd = {.op = GE_REPLACE_KEEP_EDGES,
                      .u.replace_keep_edges = {
                          .vt = vt,
                          .state_size = state_size,
                          .node_id = node_id,
                          .new_nInputs = nin,
                          .new_nOutputs = nout,
                          .initial_state = initial_state_copy,
                          .initial_state_size = initial_state_size,

                      }};
  if (!geq_push(lg->graphEditQueue, &cmd)) {
    // Queue push failed - clean up copied memory
    if (initial_state_copy) {
      free(initial_state_copy);
    }
    return false;
  }
  return true;
}

void retire_later(LiveGraph *lg, void *ptr, void (*deleter)(void *)) {
  if (!ptr)
    return;
  if (lg->retire_count >= lg->retire_capacity) {
    lg->retire_capacity = lg->retire_capacity ? lg->retire_capacity * 2 : 16;
    lg->retire_list =
        realloc(lg->retire_list, lg->retire_capacity * sizeof(RetireEntry));
  }
  lg->retire_list[lg->retire_count++] =
      (RetireEntry){.ptr = ptr, .deleter = deleter};
}

// ===================== Watch List Implementation =====================

bool add_node_to_watchlist(LiveGraph *lg, int node_id) {
  if (!lg || node_id < 0)
    return false;
  GraphEditCmd cmd = {.op = GE_ADD_WATCH, .u.add_watch = {.node_id = node_id}};
  return geq_push(lg->graphEditQueue, &cmd);
}

bool remove_node_from_watchlist(LiveGraph *lg, int node_id) {
  if (!lg || node_id < 0)
    return false;
  GraphEditCmd cmd = {
      .op = GE_REMOVE_WATCH, .u.remove_watch = {.node_id = node_id}};
  return geq_push(lg->graphEditQueue, &cmd);
}

void *get_node_state(LiveGraph *lg, int node_id, size_t *state_size) {
  if (!lg || node_id < 0 || node_id >= lg->node_capacity) {
    if (state_size)
      *state_size = 0;
    return NULL;
  }

  pthread_rwlock_rdlock(&lg->state_store_lock);

  void *result = NULL;
  size_t size = 0;

  if (lg->state_snapshots[node_id] && lg->state_sizes[node_id] > 0) {
    size = lg->state_sizes[node_id];
    result = malloc(size);
    if (result) {
      memcpy(result, lg->state_snapshots[node_id], size);
    }
  }

  pthread_rwlock_unlock(&lg->state_store_lock);

  if (state_size)
    *state_size = size;
  return result;
}
