#ifndef GRAPH_ENGINE_H
#define GRAPH_ENGINE_H

#include "graph_types.h"

// ===================== Runtime Graph Types =====================

typedef struct RTNode {
    uint64_t   logical_id;         // stable ID for migration/params
    NodeVTable vtable;
    void*      state;              // aligned, preallocated
    int        nInputs, nOutputs;
    int32_t*   inEdges;            // indices into GraphState.edgeBufs
    int32_t*   outEdges;
    int32_t*   succ;               // successor node indices
    int        succCount;          // number of nodes that depend on this node's output
    int        faninBase;          // initial number of preds
} RTNode;

typedef struct GraphState {
    // immutable after build
    RTNode*    nodes;
    int        nodeCount;
    float**    edgeBufs;           // edge buffers (mono), size=edgeCount
    int        edgeCount;
    int        maxBlock;
    int        masterEdge;         // index of master out buffer

    // per-block scheduling state
    atomic_int* pending;           // size=nodeCount
    MPMCQueue*  readyQueue;        // MPMC work queue for thread-safe job distribution
    _Atomic int jobsInFlight;

    // parameter mailbox (SPSC) for this graph
    ParamRing* params;

    // debugging label
    const char* label;
} GraphState;

// ===================== Live Editing System =====================

typedef struct LiveConnection {
    int source_node_id;
    int dest_node_id; 
    int edge_buffer_id;
    bool active;
} LiveConnection;

typedef struct LiveGraph {
    RTNode* nodes;
    int node_count, node_capacity;
    
    // Dynamic edge pool
    float** edge_buffers;
    bool* edge_free;         // which edges are available
    int edge_capacity;
    int block_size;
    
    // Live connections
    LiveConnection* connections;
    int connection_count, connection_capacity;
    
    // Orphaned nodes (have no inputs but aren't true sources)
    bool* is_orphaned;
    
    // Scheduling state (same as GraphState)
    atomic_int* pending;
    MPMCQueue* readyQueue;         // MPMC work queue for thread-safe job distribution
    _Atomic int jobsInFlight;
    
    // Parameter mailbox
    ParamRing* params;
    
    // DAC output sink - the final destination for all audio
    int dac_node_id;  // -1 if no DAC connected
    
    const char* label;
} LiveGraph;

// ===================== Worker Pool / Engine =====================

typedef struct Engine {
    _Atomic(GraphState*) current;   // current graph
    _Atomic(GraphState*) prev;      // previous graph (for crossfade), can be NULL

    _Atomic int crossfade_blocks_left; // remaining blocks of crossfade
    int crossfade_len; // total blocks to crossfade when swapping

    pthread_t* threads;
    int        workerCount;
    _Atomic int runFlag;

    _Atomic(LiveGraph*) workSession; // when non-NULL, workers process this live graph

    int sampleRate;
    int blockSize;
} Engine;

// ===================== Ready Queue Operations =====================

// Legacy ring buffer functions (replaced by MPMC queue)
bool rb_push_mpsc(GraphState* g, int32_t v);  // Deprecated: use mpmc_push(g->readyQueue, v)
bool rb_pop_sc(GraphState* g, int32_t* out);  // Deprecated: use mpmc_pop(g->readyQueue, out)

// ===================== Graph Management =====================

GraphState* alloc_graph(int nodeCount, int edgeCount, int maxBlock, const char* label);
void free_graph(GraphState* g);
void migrate_state(GraphState* newg, GraphState* oldg);

// ===================== Block Processing =====================

void process_block_parallel(GraphState* g, int nframes);
void process_block_single(GraphState* g, int nframes);
void bind_and_run(GraphState* g, int nid, int nframes);

// ===================== Worker Pool Management =====================

void engine_start_workers(int workers);
void engine_stop_workers(void);
void apply_params(GraphState* g);

// ===================== Live Graph Operations =====================

LiveGraph* create_live_graph(int initial_capacity, int block_size, const char* label);
int live_add_node(LiveGraph* lg, NodeVTable vtable, void* state, uint64_t logical_id, const char* name);
int live_add_oscillator(LiveGraph* lg, float freq_hz, const char* name);
int live_add_gain(LiveGraph* lg, float gain_value, const char* name);
int live_add_mixer2(LiveGraph* lg, const char* name);
int live_add_mixer8(LiveGraph* lg, const char* name);
int live_add_dac(LiveGraph* lg, const char* name);
bool live_connect(LiveGraph* lg, int source_id, int dest_id);
bool live_disconnect(LiveGraph* lg, int source_id, int dest_id);
void process_live_block(LiveGraph* lg, int nframes);
int find_live_output(LiveGraph* lg);

// ===================== Live Engine Operations =====================

void process_next_block(LiveGraph* lg, float* output_buffer, int nframes);

// ===================== Global Engine Instance =====================

extern Engine g_engine;

#endif // GRAPH_ENGINE_H