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
  int src_node;   // who writes this edge
  int src_port;   // which output port
} LiveEdge;

typedef struct RTNode {
  uint64_t logical_id; // stable ID for migration/params
  NodeVTable vtable;
  void *state; // aligned, preallocated
  int nInputs, nOutputs;

  // Port-based edge management
  int32_t
      *inEdgeId; // array[nInputs]: edge ID per input port (-1 if unconnected)
  int32_t *
      outEdgeId; // array[nOutputs]: edge ID per output port (-1 if unconnected)

  // For auto-sum: for each input port on this node, store an optional SUM node id
  int32_t *fanin_sum_node_id;  // array[nInputs]: SUM node ID per input port (-1 if none)

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
  int *indegree; // maintained incrementally at edits for port-based system
  MPMCQueue *readyQueue; // MPMC work queue for thread-safe job distribution
  _Atomic int jobsInFlight;

  // Parameter mailbox
  ParamRing *params;

  // DAC output sink - the final destination for all audio
  int dac_node_id; // -1 if no DAC connected

  const char *label;

  // Graph Edit Queue
  GraphEditQueue *graphEditQueue;
  
  // Failed operation tracking
  uint64_t *failed_ids;      // Array of node IDs that failed to create
  int failed_ids_count;      // Number of failed IDs
  int failed_ids_capacity;   // Capacity of failed_ids array
  
  // Atomic node ID allocation
  _Atomic int next_node_id;  // Next node ID to allocate (thread-safe)
} LiveGraph;

// ===================== Worker Pool / Engine =====================

typedef struct Engine {
  int crossfade_len; // total blocks to crossfade when swapping

  pthread_t *threads;
  int workerCount;
  _Atomic int runFlag;

  _Atomic(LiveGraph *)
      workSession; // when non-NULL, workers process this live graph

  int sampleRate;
  int blockSize;
} Engine;

// ===================== Ready Queue Operations =====================

// ===================== Graph Management =====================

// ===================== Block Processing =====================

// ===================== Worker Pool Management =====================

void engine_start_workers(int workers);
void engine_stop_workers(void);
void apply_params(LiveGraph *g);

// ===================== Live Graph Operations =====================

LiveGraph *create_live_graph(int initial_capacity, int block_size,
                             const char *label);
void destroy_live_graph(LiveGraph *lg);
int apply_add_node(LiveGraph *lg, NodeVTable vtable, void *state,
                   uint64_t logical_id, const char *name, int nInputs, int nOutputs);
int live_add_oscillator(LiveGraph *lg, float freq_hz, const char *name);
int live_add_gain(LiveGraph *lg, float gain_value, const char *name);
int live_add_number(LiveGraph *lg, float value, const char *name);
int live_add_mixer2(LiveGraph *lg, const char *name);
int live_add_mixer8(LiveGraph *lg, const char *name);
int live_add_sum(LiveGraph *lg, const char *name, int nInputs);
bool apply_connect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                   int dst_port);
bool apply_disconnect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                      int dst_port);
bool apply_delete_node(LiveGraph *lg, int node_id);
void process_live_block(LiveGraph *lg, int nframes);

// ===================== Queue-based API (Pre-allocated IDs) =====================
int add_node(LiveGraph *lg, NodeVTable vtable, void *state, const char *name, int nInputs, int nOutputs);
bool delete_node(LiveGraph *lg, int node_id);
bool connect(LiveGraph *lg, int src_node, int src_port, int dst_node, int dst_port);
bool disconnect(LiveGraph *lg, int src_node, int src_port, int dst_node, int dst_port);
bool is_failed_node(LiveGraph *lg, int node_id);
void add_failed_id(LiveGraph *lg, uint64_t logical_id);
int find_live_output(LiveGraph *lg);

// ===================== Live Engine Operations =====================

void process_next_block(LiveGraph *lg, float *output_buffer, int nframes);

// ===================== Global Engine Instance =====================

extern Engine g_engine;
void initialize_engine(int block_size, int sample_rate);

#endif // GRAPH_ENGINE_H
