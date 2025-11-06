#ifndef GRAPH_ENGINE_H
#define GRAPH_ENGINE_H

#include "graph_types.h"

// ===================== Runtime Graph Types =====================

// Per edge (really: "signal" on an output port)
typedef struct {
  float *buf;   // size = block_size
  int refcount; // number of input ports consuming this signal
  bool in_use;
} EdgeBuf;

// Live graph edge buffer (compatible with EdgeBuf but for LiveGraph)
typedef struct {
  float *buf;   // size = block_size
  int refcount; // number of input ports consuming this signal
  bool in_use;
  int src_node; // who writes this edge
  int src_port; // which output port
} LiveEdge;

typedef struct RTNode {
  uint64_t logical_id; // stable ID for migration/params
  NodeVTable vtable;
  void *state;       // aligned, preallocated
  size_t state_size; // size of allocated state for watch list copying
  int nInputs, nOutputs;

  // Port-based edge management
  int32_t
      *inEdgeId; // array[nInputs]: edge ID per input port (-1 if unconnected)
  int32_t *
      outEdgeId; // array[nOutputs]: edge ID per output port (-1 if unconnected)

  // For auto-sum: for each input port on this node, store an optional SUM node
  // id
  int32_t *fanin_sum_node_id; // array[nInputs]: SUM node ID per input port (-1
                              // if none)

  // scheduling
  int32_t *succ; // successor node indices
  int succCount; // number of nodes that depend on this node's output
} RTNode;

typedef struct GraphState {
  // immutable after build
  RTNode *nodes;
  int nodeCount;
  float **edgeBufs; // edge buffers (mono), size=edgeCount
  int edgeCount;
  int maxBlock;
  int masterEdge; // index of master out buffer

  // per-block scheduling state
  atomic_int *pending;   // size=nodeCount
  MPMCQueue *readyQueue; // MPMC work queue for thread-safe job distribution
  _Atomic int jobsInFlight;

  // parameter mailbox (SPSC) for this graph
  ParamRing *params;

  // debugging label
  const char *label;
} GraphState;

void free_graph(GraphState *g);

// ===================== Live Editing System =====================

typedef struct RetireEntry {
  void *ptr;
  void (*deleter)(void *); // e.g., free; or custom
} RetireEntry;

typedef struct LiveGraph {
  RTNode *nodes;
  int node_count, node_capacity;

  // Dynamic edge pool (new port-based system)
  LiveEdge *edges; // edge pool with refcounting
  int edge_capacity;
  int block_size;

  // Support buffers for port system
  float *silence_buf;  // zero buffer for unconnected inputs
  float *scratch_null; // throwaway buffer for unconnected outputs

  // Orphaned nodes (have no inputs but aren't true sources)
  bool *is_orphaned;

  // Scheduling state (same as GraphState)
  atomic_int *pending;
  int *indegree;      // maintained incrementally at edits for port-based system
  ReadyQ *readyQueue; // Ready queue with counting length and semaphore
  _Atomic int jobsInFlight;

  // Parameter mailbox
  ParamRing *params;

  // DAC output sink - the final destination for all audio
  int dac_node_id;  // -1 if no DAC connected
  int num_channels; // Number of output channels (1=mono, 2=stereo, etc.)

  const char *label;

  // Graph Edit Queue
  GraphEditQueue *graphEditQueue;

  // one-block retire list (old states, optional wrappers)
  RetireEntry *retire_list;
  int retire_count;
  int retire_capacity;

  // Failed operation tracking
  uint64_t *failed_ids;    // Array of node IDs that failed to create
  int failed_ids_count;    // Number of failed IDs
  int failed_ids_capacity; // Capacity of failed_ids array

  // Atomic node ID allocation
  _Atomic int next_node_id; // Next node ID to allocate (thread-safe)

  // Watch list system for state monitoring
  int *watch_list;                  // Array of node IDs being watched
  int watch_list_count;             // Current number of watched nodes
  int watch_list_capacity;          // Allocated capacity for watch list
  pthread_mutex_t watch_list_mutex; // Protects watch_list modifications

  // Thread-safe state store for watched nodes
  void **state_snapshots;            // Array of state copies indexed by node_id
  size_t *state_sizes;               // Array of state sizes indexed by node_id
  pthread_rwlock_t state_store_lock; // Reader-writer lock for state access
} LiveGraph;

// ===================== Worker Pool / Engine =====================

typedef struct Engine {
  pthread_t *threads;
  int workerCount;
  _Atomic int runFlag; // 1 = running, 0 = shutdown

  _Atomic(LiveGraph *) workSession; // published at block start, NULL after
  _Atomic int sessionFrames;        // number of frames for current block

  // Block-start wake mechanism
  pthread_mutex_t sess_mtx; // protects sess_cv wait/signal
  pthread_cond_t sess_cv;   // workers sleep here between blocks

  int sampleRate;
  int blockSize;

  // Optional: Audio Workgroup token for co-scheduling (Apple-only usage)
  // Stored as opaque pointer to avoid hard dependency in public header.
  _Atomic(void *) oswg; // os_workgroup_t when available
  _Atomic int oswg_join_pending; // set to 1 to wake workers for workgroup join
  _Atomic int oswg_join_remaining; // count of workers that need to see the flag
  _Atomic int rt_log;   // enable lightweight debug prints from workers
  _Atomic int rt_time_constraint; // apply Mach RT time-constraint policy
} Engine;

// ===================== Ready Queue Operations =====================

// ===================== Graph Management =====================

// ===================== Block Processing =====================

// ===================== Worker Pool Management =====================

void engine_start_workers(int workers);
void engine_stop_workers(void);
void apply_params(LiveGraph *g);

// Optional: supply an OS Workgroup object
// (kAudioOutputUnitProperty_OSWorkgroup) Pass the os_workgroup_t you obtained
// from the audio unit. No-ops on platforms without OS Workgroup support.
void engine_set_os_workgroup(void *oswg);
void engine_clear_os_workgroup(void);

// Enable or disable minimal worker join logging (off by default).
void engine_enable_rt_logging(int enable);

// Enable/disable Mach time-constraint scheduling for workers (Apple only).
void engine_enable_rt_time_constraint(int enable);

// ===================== Live Graph Operations =====================

LiveGraph *create_live_graph(int initial_capacity, int block_size,
                             const char *label, int num_channels);
void destroy_live_graph(LiveGraph *lg);
int apply_add_node(LiveGraph *lg, NodeVTable vtable, size_t state_size,
                   uint64_t logical_id, const char *name, int nInputs,
                   int nOutputs, const void *initial_state);
bool apply_connect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                   int dst_port);
bool apply_disconnect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                      int dst_port);
bool apply_delete_node(LiveGraph *lg, int node_id);
void process_live_block(LiveGraph *lg, int nframes);

// ===================== Queue-based API (Pre-allocated IDs)
// =====================
int add_node(LiveGraph *lg, NodeVTable vtable, size_t state_size,
             const char *name, int nInputs, int nOutputs,
             const void *initial_state, size_t initial_state_size);
bool delete_node(LiveGraph *lg, int node_id);
bool graph_connect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                   int dst_port);
bool graph_disconnect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                      int dst_port);

bool hot_swap_node(LiveGraph *lg, int node_id, NodeVTable vt, size_t state_size,
                   int nin, int nout, bool xfade,
                   void (*migrate)(void *, void *), const void *initial_state,
                   size_t initial_state_size);

bool replace_keep_edges(LiveGraph *lg, int node_id, NodeVTable vt,
                        size_t state_size, int nin, int nout, bool xfade,
                        void (*migrate)(void *, void *),
                        const void *initial_state, size_t initial_state_size);

bool is_failed_node(LiveGraph *lg, int node_id);
void add_failed_id(LiveGraph *lg, uint64_t logical_id);
int find_live_output(LiveGraph *lg);

// ===================== Live Engine Operations =====================

void process_next_block(LiveGraph *lg, float *output_buffer, int nframes);
void retire_later(LiveGraph *lg, void *ptr, void (*deleter)(void *));

// ===================== Watch List API =====================

bool add_node_to_watchlist(LiveGraph *lg, int node_id);
bool remove_node_from_watchlist(LiveGraph *lg, int node_id);
void *get_node_state(LiveGraph *lg, int node_id, size_t *state_size);

void update_orphaned_status(LiveGraph *lg);

// ===================== VTable Creation Functions =====================

// ===================== Global Engine Instance =====================

extern Engine g_engine;
void initialize_engine(int block_size, int sample_rate);

static bool ensure_port_arrays(RTNode *n) {
  // Validate node before attempting memory allocation
  if (!n || n->nInputs < 0 || n->nOutputs < 0) {
    // Invalid node state - return failure
    return false;
  }

  // Check for reasonable limits to prevent corruption-induced huge allocations
  if (n->nInputs > 1000 || n->nOutputs > 1000) {
    // Likely corrupted node - return failure
    return false;
  }

  if (!n->inEdgeId && n->nInputs > 0) {
    n->inEdgeId = (int32_t *)malloc(sizeof(int32_t) * n->nInputs);
    if (!n->inEdgeId) {
      // Malloc failed - this indicates deeper problems
      return false;
    }
    for (int i = 0; i < n->nInputs; i++)
      n->inEdgeId[i] = -1;
  }

  if (!n->outEdgeId && n->nOutputs > 0) {
    n->outEdgeId = (int32_t *)malloc(sizeof(int32_t) * n->nOutputs);
    if (!n->outEdgeId) {
      // Malloc failed - this indicates deeper problems
      return false;
    }
    for (int i = 0; i < n->nOutputs; i++)
      n->outEdgeId[i] = -1;
  }

  if (!n->fanin_sum_node_id && n->nInputs > 0) {
    n->fanin_sum_node_id = (int32_t *)malloc(sizeof(int32_t) * n->nInputs);
    if (!n->fanin_sum_node_id) {
      // Malloc failed - this is less critical but still indicates problems
      return false;
    }
    for (int i = 0; i < n->nInputs; i++)
      n->fanin_sum_node_id[i] = -1;
  }

  return true;
}

// Allocate (or reuse from pool) an edge buffer; returns edge id or -1
static int alloc_edge(LiveGraph *lg) {
  for (int i = 0; i < lg->edge_capacity; i++) {
    if (!lg->edges[i].in_use) {
      lg->edges[i].in_use = true;
      lg->edges[i].refcount = 0;
      lg->edges[i].src_node = -1;
      lg->edges[i].src_port = -1;
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

#endif // GRAPH_ENGINE_H
