// engine_demo.c - Single-file C11 demo of a real-time-ish DSP graph scheduler
// Features:
//  - DAG of nodes (kernels) with buffer routing
//  - Multithreaded execution using a worker pool (pthreads)
//  - Lock-free MPSC ready-queue, countdown dependency scheduling
//  - Parameter mailbox (SPSC) for real-time control
//  - Hot-swap graph with state migration and short crossfade between versions
//  - Pure C (no C++), portable to macOS/Linux. For iOS dynamic loading is restricted (use interpreter/JIT-less path).

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

// ===================== Helpers =====================
static void* alloc_aligned(size_t alignment, size_t size){
    void* p = NULL;
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}
static uint64_t nsec_now(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + ts.tv_nsec;
}

// ===================== Kernel ABI =====================
typedef void (*KernelFn)(float* const* in, float* const* out, int nframes, void* state);
typedef void (*InitFn)(void* state, int sampleRate, int maxBlock);
typedef void (*ResetFn)(void* state);
typedef void (*MigrateFn)(void* newState, const void* oldState);

typedef struct {
    KernelFn  process;
    InitFn    init;      // optional
    ResetFn   reset;     // optional
    MigrateFn migrate;   // optional: copy persistent state on graph swap
} NodeVTable;

// ===================== Graph Types =====================
#define MAX_IO 8

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
    int32_t*    readyRing;         // ring buffer for ready node indices
    uint32_t    readyMask;         // capacity = mask+1 (power of 2)
    _Atomic uint32_t head;         // producers push
    _Atomic uint32_t tail;         // consumers pop
    _Atomic int      jobsInFlight;

    // parameter mailbox (SPSC) for this graph
    struct ParamRing* params;

    // debugging label
    const char* label;
} GraphState;

// ===================== Parameter Mailbox =====================
// Simple SPSC ring for control messages (producer: UI/control thread; consumer: audio thread)
typedef enum { PARAM_SET_GAIN = 1 } ParamKind;

typedef struct {
    ParamKind kind;
    uint64_t  logical_id;  // target node
    float     fvalue;      // e.g., new gain
} ParamMsg;

#define PARAM_RING_CAP  256

typedef struct ParamRing {
    ParamMsg  buf[PARAM_RING_CAP];
    _Atomic uint32_t head; // producer writes
    _Atomic uint32_t tail; // consumer reads
} ParamRing;

static inline bool params_push(ParamRing* r, ParamMsg m){
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    if ((h - t) >= PARAM_RING_CAP) return false; // full
    r->buf[h % PARAM_RING_CAP] = m;
    atomic_store_explicit(&r->head, h+1, memory_order_release);
    return true;
}

static inline bool params_pop(ParamRing* r, ParamMsg* out){
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (t == h) return false; // empty
    *out = r->buf[t % PARAM_RING_CAP];
    atomic_store_explicit(&r->tail, t+1, memory_order_release);
    return true;
}

// ===================== Ready Queue (MPSC) =====================
static inline bool rb_push_mpsc(GraphState* g, int32_t v){
    uint32_t head = atomic_load_explicit(&g->head, memory_order_relaxed);
    uint32_t next = head + 1;
    if ((next - atomic_load_explicit(&g->tail, memory_order_acquire)) > g->readyMask)
        return false; // full
    g->readyRing[head & g->readyMask] = v;
    atomic_store_explicit(&g->head, next, memory_order_release);
    return true;
}
static inline bool rb_pop_sc(GraphState* g, int32_t* out){
    uint32_t tail = atomic_load_explicit(&g->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&g->head, memory_order_acquire)) return false; // empty
    *out = g->readyRing[tail & g->readyMask];
    atomic_store_explicit(&g->tail, tail+1, memory_order_release);
    return true;
}

// ===================== High-Level Graph Builder API =====================

// Higher-level node handle for Web Audio-style API
typedef struct AudioNode {
    uint64_t logical_id;
    NodeVTable vtable;
    void* state;
    
    // Connection tracking (build-time only)
    struct AudioNode** inputs;   // nodes feeding into this one
    struct AudioNode** outputs;  // nodes this one feeds
    int input_count, output_count;
    int input_capacity, output_capacity;
    
    // Metadata
    const char* name;
} AudioNode;

// Graph builder context
typedef struct GraphBuilder {
    AudioNode** nodes;
    int node_count;
    int node_capacity;
} GraphBuilder;

static uint64_t g_next_node_id = 0x1000;

static GraphBuilder* create_graph_builder() {
    GraphBuilder* gb = calloc(1, sizeof(GraphBuilder));
    gb->node_capacity = 16;
    gb->nodes = calloc(gb->node_capacity, sizeof(AudioNode*));
    return gb;
}

static void add_node_to_builder(GraphBuilder* gb, AudioNode* node) {
    if (gb->node_count >= gb->node_capacity) {
        gb->node_capacity *= 2;
        gb->nodes = realloc(gb->nodes, gb->node_capacity * sizeof(AudioNode*));
    }
    gb->nodes[gb->node_count++] = node;
}

static int find_node_index(GraphBuilder* gb, AudioNode* target) {
    for (int i = 0; i < gb->node_count; i++) {
        if (gb->nodes[i] == target) return i;
    }
    return -1; // not found
}

// ===================== Example Kernels =====================
typedef struct { float phase, inc; } OscState;
static void osc_init(void* st, int sr, int maxBlock){ (void)sr; (void)maxBlock; OscState* s=(OscState*)st; s->phase=0.f; }
static void osc_process(float* const* in, float* const* out, int n, void* st){ (void)in; OscState* s=(OscState*)st; float* y=out[0];
    for(int i=0;i<n;i++){ y[i]=2.0f*s->phase-1.0f; s->phase+=s->inc; if(s->phase>=1.f) s->phase-=1.f; }
}
static void osc_migrate(void* newState, const void* oldState){ const OscState* o=(const OscState*)oldState; OscState* n=(OscState*)newState; n->phase=o->phase; }

static const NodeVTable OSC_VTABLE = { .process=osc_process, .init=osc_init, .reset=NULL, .migrate=osc_migrate };

typedef struct { float g; } GainState;
static void gain_process(float* const* in, float* const* out, int n, void* st){ float g=((GainState*)st)->g; const float* a=in[0]; float* y=out[0]; for(int i=0;i<n;i++) y[i]=a[i]*g; }
static const NodeVTable GAIN_VTABLE = { .process=gain_process, .init=NULL, .reset=NULL, .migrate=NULL };

static void mix2_process(float* const* in, float* const* out, int n, void* st){ (void)st; const float* a=in[0]; const float* b=in[1]; float* y=out[0]; for(int i=0;i<n;i++) y[i]=a[i]+b[i]; }
static const NodeVTable MIX2_VTABLE = { .process=mix2_process, .init=NULL, .reset=NULL, .migrate=NULL };

static void mix3_process(float* const* in, float* const* out, int n, void* st){
    (void)st; const float* a=in[0]; const float* b=in[1]; const float* c=in[2]; float* y=out[0];
    for(int i=0;i<n;i++) y[i]=a[i]+b[i]+c[i];
}

// ===================== Web Audio-Style Node Creation =====================

// Create an oscillator node
static AudioNode* create_oscillator(GraphBuilder* gb, float freq_hz, const char* name) {
    AudioNode* node = calloc(1, sizeof(AudioNode));
    node->logical_id = ++g_next_node_id;
    node->vtable = OSC_VTABLE;
    node->name = name;
    
    // Allocate and initialize state
    OscState* state = calloc(1, sizeof(OscState));
    state->inc = freq_hz / 48000.0f; // assume 48kHz for now
    state->phase = 0.0f;
    node->state = state;
    
    add_node_to_builder(gb, node);
    return node;
}

// Create a gain node
static AudioNode* create_gain(GraphBuilder* gb, float gain_value, const char* name) {
    AudioNode* node = calloc(1, sizeof(AudioNode));
    node->logical_id = ++g_next_node_id;
    node->vtable = GAIN_VTABLE;
    node->name = name;
    
    // Allocate and initialize state
    GainState* state = calloc(1, sizeof(GainState));
    state->g = gain_value;
    node->state = state;
    
    add_node_to_builder(gb, node);
    return node;
}

// Create a 2-input mixer node
static AudioNode* create_mixer2(GraphBuilder* gb, const char* name) {
    AudioNode* node = calloc(1, sizeof(AudioNode));
    node->logical_id = ++g_next_node_id;
    node->vtable = MIX2_VTABLE;
    node->name = name;
    node->state = NULL; // mixer doesn't need state
    
    add_node_to_builder(gb, node);
    return node;
}

// Create a 3-input mixer node
static AudioNode* create_mixer3(GraphBuilder* gb, const char* name) {
    AudioNode* node = calloc(1, sizeof(AudioNode));
    node->logical_id = ++g_next_node_id;
    node->vtable = (NodeVTable){ .process = mix3_process, .init = NULL, .reset = NULL, .migrate = NULL };
    node->name = name;
    node->state = NULL; // mixer doesn't need state
    
    add_node_to_builder(gb, node);
    return node;
}

// The Web Audio-style connect function!
static void connect(AudioNode* source, AudioNode* dest) {
    // Add dest to source's outputs
    if (source->output_count >= source->output_capacity) {
        source->output_capacity = source->output_capacity ? source->output_capacity * 2 : 4;
        source->outputs = realloc(source->outputs, source->output_capacity * sizeof(AudioNode*));
    }
    source->outputs[source->output_count++] = dest;
    
    // Add source to dest's inputs  
    if (dest->input_count >= dest->input_capacity) {
        dest->input_capacity = dest->input_capacity ? dest->input_capacity * 2 : 4;
        dest->inputs = realloc(dest->inputs, dest->input_capacity * sizeof(AudioNode*));
    }
    dest->inputs[dest->input_count++] = source;
}

// Count total connections to determine edge buffer count
static int count_total_edges(GraphBuilder* gb) {
    int total = 0;
    for (int i = 0; i < gb->node_count; i++) {
        total += gb->nodes[i]->output_count;
    }
    return total;
}

// Find the output node (node with no outputs) for master edge
static AudioNode* find_output_node(GraphBuilder* gb) {
    for (int i = 0; i < gb->node_count; i++) {
        if (gb->nodes[i]->output_count == 0) {
            return gb->nodes[i];
        }
    }
    return NULL; // no output found
}

// Forward declaration for alloc_graph (defined later)
static GraphState* alloc_graph(int nodeCount, int edgeCount, int maxBlock, const char* label);

// Compile the high-level graph into the low-level runtime format
static GraphState* compile_graph(GraphBuilder* gb, int sample_rate, int block_size, const char* label) {
    int edge_count = count_total_edges(gb);
    GraphState* g = alloc_graph(gb->node_count, edge_count, block_size, label);
    
    // First pass: allocate edge indices for each connection
    int edge_idx = 0;
    for (int i = 0; i < gb->node_count; i++) {
        AudioNode* node = gb->nodes[i];
        
        // Allocate edge indices for this node's outputs
        for (int j = 0; j < node->output_count; j++) {
            // Store edge index in a temporary way - we'll use a simple scheme
            // where we assign edges sequentially and track them
            edge_idx++;
        }
    }
    
    // Second pass: convert high-level nodes to RTNodes
    edge_idx = 0;
    for (int i = 0; i < gb->node_count; i++) {
        AudioNode* hn = gb->nodes[i];  // high-level node
        RTNode* rn = &g->nodes[i];     // runtime node
        
        // Basic node properties
        rn->logical_id = hn->logical_id;
        rn->vtable = hn->vtable;
        rn->state = hn->state;  // transfer ownership
        rn->nInputs = hn->input_count;
        rn->nOutputs = hn->output_count;
        rn->faninBase = hn->input_count;  // dependency count
        rn->succCount = hn->output_count;
        
        // Allocate and populate input edges
        if (rn->nInputs > 0) {
            rn->inEdges = malloc(rn->nInputs * sizeof(int32_t));
            for (int j = 0; j < rn->nInputs; j++) {
                // Find which edge connects this input
                AudioNode* source = hn->inputs[j];
                int source_idx = find_node_index(gb, source);
                
                // Find which output of the source connects to this input
                int output_slot = -1;
                for (int k = 0; k < source->output_count; k++) {
                    if (source->outputs[k] == hn) {
                        output_slot = k;
                        break;
                    }
                }
                
                // Calculate edge index: sum of outputs from previous nodes + output slot
                int source_edge_base = 0;
                for (int n = 0; n < source_idx; n++) {
                    source_edge_base += gb->nodes[n]->output_count;
                }
                rn->inEdges[j] = source_edge_base + output_slot;
            }
        }
        
        // Allocate and populate output edges
        if (rn->nOutputs > 0) {
            rn->outEdges = malloc(rn->nOutputs * sizeof(int32_t));
            int node_edge_base = 0;
            for (int n = 0; n < i; n++) {
                node_edge_base += gb->nodes[n]->output_count;
            }
            for (int j = 0; j < rn->nOutputs; j++) {
                rn->outEdges[j] = node_edge_base + j;
            }
        }
        
        // Allocate and populate successor indices
        if (rn->succCount > 0) {
            rn->succ = malloc(rn->succCount * sizeof(int32_t));
            for (int j = 0; j < rn->succCount; j++) {
                rn->succ[j] = find_node_index(gb, hn->outputs[j]);
            }
        }
    }
    
    // Find master output edge (from the output node)
    AudioNode* output_node = find_output_node(gb);
    if (output_node) {
        int output_idx = find_node_index(gb, output_node);
        if (output_idx >= 0 && g->nodes[output_idx].nInputs > 0) {
            g->masterEdge = g->nodes[output_idx].inEdges[0]; // use first input as master
        }
    }
    
    return g;
}

// Clean up the graph builder
static void free_graph_builder(GraphBuilder* gb) {
    for (int i = 0; i < gb->node_count; i++) {
        AudioNode* node = gb->nodes[i];
        free(node->inputs);
        free(node->outputs);
        // Note: don't free node->state - it's transferred to the compiled graph
        free(node);
    }
    free(gb->nodes);
    free(gb);
}

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
    int32_t* readyRing;
    uint32_t readyMask;
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    _Atomic int jobsInFlight;
    
    // Parameter mailbox
    ParamRing* params;
    
    const char* label;
} LiveGraph;

static LiveGraph* create_live_graph(int initial_capacity, int block_size, const char* label) {
    LiveGraph* lg = calloc(1, sizeof(LiveGraph));
    
    // Node storage
    lg->node_capacity = initial_capacity;
    lg->nodes = calloc(lg->node_capacity, sizeof(RTNode));
    lg->pending = calloc(lg->node_capacity, sizeof(atomic_int));
    lg->is_orphaned = calloc(lg->node_capacity, sizeof(bool));
    
    // Edge pool (start with generous capacity)
    lg->edge_capacity = initial_capacity * 4;
    lg->edge_buffers = calloc(lg->edge_capacity, sizeof(float*));
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
    
    // Ready queue
    lg->readyMask = 1024 - 1;
    lg->readyRing = calloc(lg->readyMask + 1, sizeof(int32_t));
    
    // Parameter mailbox
    lg->params = calloc(1, sizeof(ParamRing));
    
    lg->label = label;
    return lg;
}

static int find_free_edge(LiveGraph* lg) {
    for (int i = 0; i < lg->edge_capacity; i++) {
        if (lg->edge_free[i]) {
            lg->edge_free[i] = false;
            printf("  Allocated edge %d\n", i);
            return i;
        }
    }
    printf("  ERROR: No free edges available!\n");
    return -1; // no free edges (should expand pool)
}

static void free_edge(LiveGraph* lg, int edge_id) {
    if (edge_id >= 0 && edge_id < lg->edge_capacity) {
        lg->edge_free[edge_id] = true;
        // Clear the buffer
        memset(lg->edge_buffers[edge_id], 0, lg->block_size * sizeof(float));
    }
}

// Add a new node to the live graph and return its ID
static int live_add_node(LiveGraph* lg, NodeVTable vtable, void* state, uint64_t logical_id, const char* name) {
    // Find free slot or expand
    int node_id = lg->node_count;
    if (node_id >= lg->node_capacity) {
        // Need to expand - for demo just fail
        return -1;
    }
    
    RTNode* node = &lg->nodes[node_id];
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
    
    lg->node_count++;
    
    printf("Added live node %d (%s)\n", node_id, name ? name : "unnamed");
    return node_id;
}

// Web Audio-style node creation functions
static int live_add_oscillator(LiveGraph* lg, float freq_hz, const char* name) {
    OscState* state = calloc(1, sizeof(OscState));
    state->inc = freq_hz / 48000.0f;
    state->phase = 0.0f;
    return live_add_node(lg, OSC_VTABLE, state, ++g_next_node_id, name);
}

static int live_add_gain(LiveGraph* lg, float gain_value, const char* name) {
    GainState* state = calloc(1, sizeof(GainState));
    state->g = gain_value;
    return live_add_node(lg, GAIN_VTABLE, state, ++g_next_node_id, name);
}

static int live_add_mixer2(LiveGraph* lg, const char* name) {
    return live_add_node(lg, MIX2_VTABLE, NULL, ++g_next_node_id, name);
}

// Helper to add edge to node's input/output arrays
static void add_input_edge(RTNode* node, int edge_id) {
    // Expand input array
    node->inEdges = realloc(node->inEdges, (node->nInputs + 1) * sizeof(int32_t));
    node->inEdges[node->nInputs] = edge_id;
    node->nInputs++;
    node->faninBase++; // more dependencies
}

static void add_output_edge(RTNode* node, int edge_id) {
    // Expand output array  
    node->outEdges = realloc(node->outEdges, (node->nOutputs + 1) * sizeof(int32_t));
    node->outEdges[node->nOutputs] = edge_id;
    node->nOutputs++;
}

static void add_successor(RTNode* node, int succ_id) {
    // Expand successor array
    node->succ = realloc(node->succ, (node->succCount + 1) * sizeof(int32_t));
    node->succ[node->succCount] = succ_id;
    node->succCount++;
}

// Live connect function - add connection while audio is running!
static bool live_connect(LiveGraph* lg, int source_id, int dest_id) {
    if (source_id < 0 || source_id >= lg->node_count || 
        dest_id < 0 || dest_id >= lg->node_count) {
        printf("ERROR: Invalid node IDs for connect: %d -> %d\n", source_id, dest_id);
        return false;
    }
    
    printf("    Attempting to connect node %d -> node %d\n", source_id, dest_id);
    if (lg->is_orphaned[source_id]) {
        printf("    WARNING: Connecting from orphaned node %d\n", source_id);
    }
    if (lg->is_orphaned[dest_id]) {
        printf("    INFO: Connecting to previously orphaned node %d\n", dest_id);
    }
    
    // Get a free edge buffer
    int edge_id = find_free_edge(lg);
    if (edge_id < 0) {
        printf("No free edges available!\n");
        return false;
    }
    
    RTNode* source = &lg->nodes[source_id];
    RTNode* dest = &lg->nodes[dest_id];
    
    // Add edge to source's outputs
    add_output_edge(source, edge_id);
    
    // Add edge to dest's inputs 
    add_input_edge(dest, edge_id);
    
    // Add dest to source's successors
    add_successor(source, dest_id);
    
    // Check if dest node is no longer orphaned (now has inputs)
    if (dest->nInputs > 0 && lg->is_orphaned[dest_id]) {
        lg->is_orphaned[dest_id] = false;
        printf("  Node %d is no longer orphaned\n", dest_id);
    }
    
    // Record the connection
    if (lg->connection_count < lg->connection_capacity) {
        LiveConnection* conn = &lg->connections[lg->connection_count++];
        conn->source_node_id = source_id;
        conn->dest_node_id = dest_id;
        conn->edge_buffer_id = edge_id;
        conn->active = true;
    }
    
    printf("Connected node %d -> node %d (edge %d)\n", source_id, dest_id, edge_id);
    return true;
}

// Live disconnect function
static bool live_disconnect(LiveGraph* lg, int source_id, int dest_id) {
    if (source_id < 0 || source_id >= lg->node_count || 
        dest_id < 0 || dest_id >= lg->node_count) {
        printf("Invalid node IDs for disconnect: %d -> %d\n", source_id, dest_id);
        return false;
    }
    
    // Find the connection
    LiveConnection* conn = NULL;
    for (int i = 0; i < lg->connection_count; i++) {
        if (lg->connections[i].source_node_id == source_id && 
            lg->connections[i].dest_node_id == dest_id &&
            lg->connections[i].active) {
            conn = &lg->connections[i];
            break;
        }
    }
    
    if (!conn) {
        printf("Connection %d -> %d not found\n", source_id, dest_id);
        return false;
    }
    
    RTNode* source = &lg->nodes[source_id];
    RTNode* dest = &lg->nodes[dest_id];
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
    
    // Check if dest node became orphaned (no inputs but has outputs)
    if (dest->nInputs == 0 && dest->nOutputs > 0) {
        lg->is_orphaned[dest_id] = true;
        printf("  Node %d is now orphaned\n", dest_id);
    }
    
    // Check if source node became isolated (no outputs)
    if (source->nOutputs == 0) {
        printf("  Node %d is now isolated (no outputs)\n", source_id);
    }
    
    // Free the edge buffer
    free_edge(lg, edge_id);
    
    // Mark connection as inactive
    conn->active = false;
    
    printf("Disconnected node %d -> node %d (freed edge %d)\n", source_id, dest_id, edge_id);
    return true;
}

// Forward declarations for live graph processing functions
static inline bool rb_push_mpsc_live(LiveGraph* lg, int32_t v);
static inline bool rb_pop_sc_live(LiveGraph* lg, int32_t* out);
static void bind_and_run_live(LiveGraph* lg, int nid, int nframes);

// Process one block on a live graph using the edge_buffers array
static void process_live_block(LiveGraph* lg, int nframes) {
    // Reset scheduling state
    atomic_store_explicit(&lg->head, 0, memory_order_relaxed);
    atomic_store_explicit(&lg->tail, 0, memory_order_relaxed);
    
    // Debug: print dependency state and adjust for orphaned nodes
    printf("    Dependency state: ");
    for (int i = 0; i < lg->node_count; i++) {
        if (lg->is_orphaned[i]) {
            // Orphaned nodes don't participate in scheduling
            atomic_store_explicit(&lg->pending[i], -1, memory_order_relaxed);
            printf("node%d=ORPHANED ", i);
        } else {
            // Calculate actual dependencies excluding orphaned predecessors
            int actual_deps = 0;
            for (int j = 0; j < lg->nodes[i].nInputs; j++) {
                // Find which node produces this input edge
                int input_edge = lg->nodes[i].inEdges[j];
                bool found_producer = false;
                for (int k = 0; k < lg->node_count; k++) {
                    if (lg->is_orphaned[k]) continue; // skip orphaned nodes
                    for (int m = 0; m < lg->nodes[k].nOutputs; m++) {
                        if (lg->nodes[k].outEdges[m] == input_edge) {
                            actual_deps++;
                            found_producer = true;
                            break;
                        }
                    }
                    if (found_producer) break;
                }
            }
            atomic_store_explicit(&lg->pending[i], actual_deps, memory_order_relaxed);
            printf("node%d=%d ", i, actual_deps);
        }
    }
    printf("\n");
    
    // Seed source nodes (nodes with no inputs AND have outputs, skip orphaned and isolated nodes)
    int totalJobs = 0;
    int sources_found = 0;
    for (int i = 0; i < lg->node_count; i++) {
        RTNode* node = &lg->nodes[i];
        
        // A true source has no inputs but DOES have outputs
        bool is_true_source = (atomic_load(&lg->pending[i]) == 0) && (node->nOutputs > 0) && !lg->is_orphaned[i];
        
        // Count jobs: exclude orphaned nodes and isolated nodes (no inputs, no outputs)
        bool is_isolated = (node->nInputs == 0) && (node->nOutputs == 0);
        bool should_process = !lg->is_orphaned[i] && !is_isolated;
        
        if (is_true_source) {
            printf("    Seeding source node %d\n", i);
            while (!rb_push_mpsc_live(lg, i)) { /* spin */ }
            sources_found++;
        } else if (atomic_load(&lg->pending[i]) == 0 && !lg->is_orphaned[i]) {
            printf("    Skipping isolated node %d (no outputs)\n", i);
        }
        
        if (should_process) {
            totalJobs++;
        }
    }
    printf("    Found %d source nodes, total jobs = %d\n", sources_found, totalJobs);
    atomic_store_explicit(&lg->jobsInFlight, totalJobs, memory_order_release);
    
    if (sources_found == 0) {
        printf("    ERROR: No source nodes found - graph has cycles or all nodes have inputs!\n");
        return;
    }
    
    // Process nodes (single-threaded for simplicity in demo)
    int32_t nid;
    int processed = 0;
    while (rb_pop_sc_live(lg, &nid)) {
        printf("    Processing node %d\n", nid);
        bind_and_run_live(lg, nid, nframes);
        RTNode* node = &lg->nodes[nid];
        
        // Notify successors
        if (node->succ && node->succCount > 0) {
            for (int i = 0; i < node->succCount; i++) {
                int succ = node->succ[i];
                printf("      Notifying successor %d\n", succ);
                if (succ >= 0 && succ < lg->node_count && !lg->is_orphaned[succ]) {
                    if (atomic_fetch_sub_explicit(&lg->pending[succ], 1, memory_order_acq_rel) == 1) {
                        printf("      Successor %d is now ready\n", succ);
                        while (!rb_push_mpsc_live(lg, succ)) { /* spin */ }
                    }
                } else {
                    printf("      Skipping invalid/orphaned successor %d\n", succ);
                }
            }
        }
        atomic_fetch_sub_explicit(&lg->jobsInFlight, 1, memory_order_acq_rel);
        processed++;
        
        if (processed > 20) {
            printf("    ERROR: Processed too many nodes, breaking to avoid infinite loop\n");
            break;
        }
    }
    printf("    Processed %d nodes total\n", processed);
}

// Helper functions for live graph processing
static inline bool rb_push_mpsc_live(LiveGraph* lg, int32_t v) {
    uint32_t head = atomic_load_explicit(&lg->head, memory_order_relaxed);
    uint32_t next = head + 1;
    if ((next - atomic_load_explicit(&lg->tail, memory_order_acquire)) > lg->readyMask)
        return false; // full
    lg->readyRing[head & lg->readyMask] = v;
    atomic_store_explicit(&lg->head, next, memory_order_release);
    return true;
}

static inline bool rb_pop_sc_live(LiveGraph* lg, int32_t* out) {
    uint32_t tail = atomic_load_explicit(&lg->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&lg->head, memory_order_acquire)) return false; // empty
    *out = lg->readyRing[tail & lg->readyMask];
    atomic_store_explicit(&lg->tail, tail + 1, memory_order_release);
    return true;
}

static void bind_and_run_live(LiveGraph* lg, int nid, int nframes) {
    if (nid < 0 || nid >= lg->node_count) {
        printf("ERROR: Invalid node ID %d\n", nid);
        return;
    }
    
    RTNode* node = &lg->nodes[nid];
    
    // Validate node data structures
    if (node->nInputs < 0 || node->nOutputs < 0 || 
        node->nInputs > MAX_IO || node->nOutputs > MAX_IO) {
        printf("ERROR: Invalid node %d I/O counts: in=%d out=%d\n", nid, node->nInputs, node->nOutputs);
        return;
    }
    
    // Skip orphaned nodes entirely - they shouldn't be in the scheduling queue anyway
    if (lg->is_orphaned[nid]) {
        printf("      Skipping orphaned node %d\n", nid);
        return;
    }
    
    // Don't process if arrays are NULL (should not happen but safety check)
    if ((node->nInputs > 0 && !node->inEdges) || (node->nOutputs > 0 && !node->outEdges)) {
        printf("ERROR: Node %d has NULL edge arrays\n", nid);
        return;
    }
    
    float* inPtrsStatic[MAX_IO];
    float* outPtrsStatic[MAX_IO];
    
    // Bounds check and populate input pointers
    for (int i = 0; i < node->nInputs && i < MAX_IO; i++) {
        int edge_id = node->inEdges[i];
        if (edge_id >= 0 && edge_id < lg->edge_capacity) {
            inPtrsStatic[i] = lg->edge_buffers[edge_id];
        } else {
            printf("ERROR: Invalid input edge %d for node %d\n", edge_id, nid);
            inPtrsStatic[i] = lg->edge_buffers[0]; // fallback to first buffer
        }
    }
    
    // Bounds check and populate output pointers  
    for (int i = 0; i < node->nOutputs && i < MAX_IO; i++) {
        int edge_id = node->outEdges[i];
        if (edge_id >= 0 && edge_id < lg->edge_capacity) {
            outPtrsStatic[i] = lg->edge_buffers[edge_id];
        } else {
            printf("ERROR: Invalid output edge %d for node %d\n", edge_id, nid);
            outPtrsStatic[i] = lg->edge_buffers[0]; // fallback to first buffer
        }
    }
    
    if (node->vtable.process) {
        node->vtable.process((float* const*)inPtrsStatic, (float* const*)outPtrsStatic, nframes, node->state);
    }
}

// Find an output node for getting final audio
static int find_live_output(LiveGraph* lg) {
    for (int i = 0; i < lg->node_count; i++) {
        if (lg->nodes[i].nOutputs == 0 && lg->nodes[i].nInputs > 0) {
            return i; // has inputs but no outputs
        }
    }
    return -1;
}

// ===================== Worker Pool / Engine =====================
typedef struct Engine {
    _Atomic(GraphState*) current;   // current graph
    _Atomic(GraphState*) prev;      // previous graph (for crossfade), can be NULL

    _Atomic int crossfade_blocks_left; // remaining blocks of crossfade
    int crossfade_len; // total blocks to crossfade when swapping

    pthread_t* threads;
    int        workerCount;
    _Atomic int runFlag;

    _Atomic(GraphState*) workSession; // when non-NULL, workers process that graph

    int sampleRate;
    int blockSize;
} Engine;

static Engine g_engine; // single global for demo

static void bind_and_run(GraphState* g, int nid, int nframes){
    RTNode* node = &g->nodes[nid];
    float* inPtrsStatic[MAX_IO];
    float* outPtrsStatic[MAX_IO];
    for(int i=0;i<node->nInputs;i++) inPtrsStatic[i] = g->edgeBufs[node->inEdges[i]];
    for(int i=0;i<node->nOutputs;i++) outPtrsStatic[i]= g->edgeBufs[node->outEdges[i]];
    node->vtable.process((float* const*)inPtrsStatic, (float* const*)outPtrsStatic, nframes, node->state);
}

static void* worker_main(void* arg){ (void)arg;
    for(;;){
        if(!atomic_load_explicit(&g_engine.runFlag, memory_order_acquire)) break;
        GraphState* g = atomic_load_explicit(&g_engine.workSession, memory_order_acquire);
        if(!g){ sched_yield(); continue; }
        int32_t nid;
        // Try to get a ready node from the work queue
        if (!rb_pop_sc(g, &nid)) { 
            // Queue is empty - use hybrid spin-yield strategy:
            
            // First, do a brief spin loop (64 iterations) to avoid syscall overhead
            // if work becomes available soon. The memory barrier prevents compiler
            // optimization from removing the loop.
            for (int i = 0; i < 64; i++) {
                __asm__ __volatile__("" ::: "memory");  // Memory barrier (no-op instruction)
            }
            
            // Still no work - cooperatively yield CPU time to other threads
            // (audio thread, other workers, or OS tasks)
            sched_yield();
            
            // Go back to top of worker loop to check for new work
            continue; 
        }
        bind_and_run(g, nid, g_engine.blockSize);
        RTNode* node=&g->nodes[nid];
        // loop through each node that depends on this node's output
        for(int i=0;i<node->succCount;i++){
            int succ=node->succ[i];
            // note: pending[succ] = how many dependencies the successor is still waiting for
            if(atomic_fetch_sub_explicit(&g->pending[succ],1,memory_order_acq_rel)==1){
                // this was the LAST dependency the successor was waiting for
                // successor is now ready to run (all its inputs are satisfied)

                // try to add successor to ready queue
                // note: this loop spins if queue if full (rare, but ensures we don't lose work)
                while(!rb_push_mpsc(g,succ)){ __asm__ __volatile__("" ::: "memory"); }
            }
        }
        // we've completed a job, so decrement jobsInFlight
        atomic_fetch_sub_explicit(&g->jobsInFlight,1,memory_order_acq_rel);
    }
    return NULL;
}

static void engine_start_workers(int workers){
    g_engine.workerCount = workers;
    g_engine.threads = (pthread_t*)calloc(workers, sizeof(pthread_t));
    atomic_store(&g_engine.runFlag, 1);
    for(int i=0;i<workers;i++){
        pthread_attr_t attr; pthread_attr_init(&attr);
        pthread_create(&g_engine.threads[i], &attr, worker_main, NULL);
        pthread_attr_destroy(&attr);
    }
}
static void engine_stop_workers(){
    atomic_store(&g_engine.runFlag, 0);
    for(int i=0;i<g_engine.workerCount;i++) pthread_join(g_engine.threads[i], NULL);
    free(g_engine.threads); g_engine.threads=NULL; g_engine.workerCount=0;
}

// Drain parameter ring and apply to nodes (simple linear search by logical_id)
static void apply_params(GraphState* g){
    if(!g || !g->params) return;
    ParamMsg m;
    while(params_pop(g->params,&m)){
        for(int i=0;i<g->nodeCount;i++){
            if(g->nodes[i].logical_id==m.logical_id){
                if(m.kind==PARAM_SET_GAIN){
                    GainState* st=(GainState*)g->nodes[i].state; st->g = m.fvalue;
                }
            }
        }
    }
}
/*
 * Relationship bewteen worker_main and process_block_parallel:
 *
  process_block_parallel is the coordinator that:
  1. Sets up the work session
  2. Also participates as a worker (work-stealing)
  3. Waits for all work to complete

  worker_main threads are dedicated workers that:
  1. Continuously look for work sessions
  2. Process nodes when work is available
  3. Go idle when no work exists
 */

// Process one block on a graph using workers + audio thread (work-stealing)
static void process_block_parallel(GraphState* g, int nframes){
    // reset ring and pending
    atomic_store_explicit(&g->head, 0, memory_order_relaxed);
    atomic_store_explicit(&g->tail, 0, memory_order_relaxed);

    for(int i=0;i<g->nodeCount;i++) atomic_store_explicit(&g->pending[i], g->nodes[i].faninBase, memory_order_relaxed);

    // seed sources
    int totalJobs=0; for(int i=0;i<g->nodeCount;i++){ if(g->nodes[i].faninBase==0) rb_push_mpsc(g,i); totalJobs++; }
    atomic_store_explicit(&g->jobsInFlight, totalJobs, memory_order_release);

    // publish work session
    atomic_store_explicit(&g_engine.workSession, g, memory_order_release);

    // audio thread helps too
    while(atomic_load_explicit(&g->jobsInFlight, memory_order_acquire) > 0){
        int32_t nid;
        // read a new job from the ready queue
        if(rb_pop_sc(g,&nid)){
            bind_and_run(g, nid, nframes);
            RTNode* node=&g->nodes[nid];
            for(int i=0;i<node->succCount;i++){
                int succ=node->succ[i];
                if(atomic_fetch_sub_explicit(&g->pending[succ],1,memory_order_acq_rel)==1){
                    while(!rb_push_mpsc(g,succ)){ __asm__ __volatile__("" ::: "memory"); }
                }
            }
            atomic_fetch_sub_explicit(&g->jobsInFlight,1,memory_order_acq_rel);
        } else {
            __asm__ __volatile__("" ::: "memory");
        }
    }
    // clear session
    atomic_store_explicit(&g_engine.workSession, NULL, memory_order_release);
}

// Process one block single-threaded (no workers) - useful for shadow old graph during crossfade
static void process_block_single(GraphState* g, int nframes){
    atomic_store_explicit(&g->head, 0, memory_order_relaxed);
    atomic_store_explicit(&g->tail, 0, memory_order_relaxed);
    for(int i=0;i<g->nodeCount;i++) atomic_store_explicit(&g->pending[i], g->nodes[i].faninBase, memory_order_relaxed);
    for(int i=0;i<g->nodeCount;i++) if(g->nodes[i].faninBase==0) rb_push_mpsc(g,i);

    int jobs=0; for(int i=0;i<g->nodeCount;i++) jobs++;
    atomic_store(&g->jobsInFlight, jobs);
    int32_t nid;
    while(rb_pop_sc(g,&nid)){
        bind_and_run(g, nid, nframes);
        RTNode* node=&g->nodes[nid];
        for(int i=0;i<node->succCount;i++){
            int succ=node->succ[i];
            if(atomic_fetch_sub_explicit(&g->pending[succ],1,memory_order_acq_rel)==1){
                while(!rb_push_mpsc(g,succ)) { __asm__ __volatile__("" ::: "memory"); }
            }
        }
    }
}

// ===================== Build / Destroy Graphs =====================
static void free_graph(GraphState* g){
    if(!g) return;
    for(int i=0;i<g->edgeCount;i++) free(g->edgeBufs[i]);
    free(g->edgeBufs);
    for(int i=0;i<g->nodeCount;i++){
        free(g->nodes[i].inEdges);
        free(g->nodes[i].outEdges);
        free(g->nodes[i].succ);
        free(g->nodes[i].state);
    }
    free(g->nodes);
    free(g->pending);
    free(g->readyRing);
    free(g->params);
    free(g);
}

static GraphState* alloc_graph(int nodeCount, int edgeCount, int maxBlock, const char* label){
    GraphState* g = (GraphState*)calloc(1,sizeof(GraphState));
    g->nodeCount=nodeCount; g->edgeCount=edgeCount; g->maxBlock=maxBlock; g->label=label;
    g->nodes=(RTNode*)calloc(nodeCount,sizeof(RTNode));
    g->pending=(atomic_int*)calloc(nodeCount,sizeof(atomic_int));
    g->edgeBufs=(float**)calloc(edgeCount,sizeof(float*));
    for(int e=0;e<edgeCount;e++) g->edgeBufs[e]=(float*)alloc_aligned(64, maxBlock*sizeof(float));
    g->readyMask = 1024-1; g->readyRing=(int32_t*)calloc(g->readyMask+1, sizeof(int32_t));
    g->params=(ParamRing*)calloc(1,sizeof(ParamRing));
    return g;
}

// Build variant 0: two osc branches -> gains -> mix -> master (Web Audio style!)
static GraphState* build_graph_v0(int sr, int block) {
    GraphBuilder* gb = create_graph_builder();
    
    // Create nodes with nice names and frequencies
    AudioNode* oscA = create_oscillator(gb, 440.0f, "oscA");    // A4 = 440 Hz
    AudioNode* oscB = create_oscillator(gb, 660.0f, "oscB");    // E5 = 660 Hz (perfect fifth)
    AudioNode* gainA = create_gain(gb, 0.5f, "gainA");
    AudioNode* gainB = create_gain(gb, 0.8f, "gainB");
    AudioNode* mixer = create_mixer2(gb, "mixer");
    
    // Connect the graph: oscA -> gainA -> mixer <- gainB <- oscB
    connect(oscA, gainA);
    connect(oscB, gainB);
    connect(gainA, mixer);
    connect(gainB, mixer);
    
    // Set specific logical IDs to match the old system (for parameter updates)
    oscA->logical_id = 0x1111;
    oscB->logical_id = 0x2222;
    gainA->logical_id = 0x3333;
    gainB->logical_id = 0x4444;
    mixer->logical_id = 0x5555;
    
    // Compile to runtime format
    GraphState* g = compile_graph(gb, sr, block, "graph_v0");
    
    // Clean up the builder (nodes transferred to compiled graph)
    free_graph_builder(gb);
    return g;
}

// Build variant 1: add third branch oscC+gainC -> mixer, tweak gains (demonstrates topology change)
static GraphState* build_graph_v1(int sr, int block) {
    GraphBuilder* gb = create_graph_builder();
    
    // Create nodes with different frequencies for a richer chord
    AudioNode* oscA = create_oscillator(gb, 440.0f, "oscA");    // A4 = 440 Hz
    AudioNode* oscB = create_oscillator(gb, 660.0f, "oscB");    // E5 = 660 Hz  
    AudioNode* oscC = create_oscillator(gb, 880.0f, "oscC");    // A5 = 880 Hz (octave)
    AudioNode* gainA = create_gain(gb, 0.7f, "gainA");         // slightly different gains
    AudioNode* gainB = create_gain(gb, 0.6f, "gainB");
    AudioNode* gainC = create_gain(gb, 0.5f, "gainC");
    AudioNode* mixer = create_mixer3(gb, "mixer3");             // 3-input mixer
    
    // Connect the graph: three oscillator chains feeding into one mixer
    connect(oscA, gainA);
    connect(oscB, gainB);
    connect(oscC, gainC);
    connect(gainA, mixer);
    connect(gainB, mixer);
    connect(gainC, mixer);
    
    // Set specific logical IDs to match the old system (for parameter updates and migration)
    oscA->logical_id = 0x1111;
    oscB->logical_id = 0x2222;
    oscC->logical_id = 0x6666;
    gainA->logical_id = 0x3333;
    gainB->logical_id = 0x4444;
    gainC->logical_id = 0x7777;
    mixer->logical_id = 0x5555;
    
    // Compile to runtime format
    GraphState* g = compile_graph(gb, sr, block, "graph_v1");
    
    // Clean up the builder
    free_graph_builder(gb);
    return g;
}

// Migrate matching nodes by logical_id (copies state via vtable.migrate if provided)
static void migrate_state(GraphState* newg, GraphState* oldg){
    if(!newg || !oldg) return;
    for(int i=0;i<newg->nodeCount;i++){
        uint64_t id = newg->nodes[i].logical_id;
        if(!newg->nodes[i].vtable.migrate) continue;
        for(int j=0;j<oldg->nodeCount;j++){
            if(oldg->nodes[j].logical_id==id){
                newg->nodes[i].vtable.migrate(newg->nodes[i].state, oldg->nodes[j].state);
                break;
            }
        }
    }
}

// ===================== Original Demo (Compiled Graphs with Hot-Swap) =====================
int main_compiled_demo(void){
    g_engine.sampleRate = 48000; g_engine.blockSize = 128; g_engine.crossfade_len = 4;

    GraphState* g0 = build_graph_v0(g_engine.sampleRate, g_engine.blockSize);
    GraphState* g1 = NULL;

    atomic_store(&g_engine.current, g0);
    atomic_store(&g_engine.prev, NULL);
    atomic_store(&g_engine.workSession, NULL);
    atomic_store(&g_engine.crossfade_blocks_left, 0);

    int cpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    int workers = cpu > 1 ? cpu-1 : 1;
    engine_start_workers(workers);

    float* outNew = (float*)calloc(g_engine.blockSize, sizeof(float));
    float* outOld = (float*)calloc(g_engine.blockSize, sizeof(float));
    float* mix    = (float*)calloc(g_engine.blockSize, sizeof(float));

    printf("\n--- Running initial graph v0 for a few blocks ---\n");
    for(int b=0;b<6;b++){
        GraphState* cg = atomic_load(&g_engine.current);
        apply_params(cg);
        process_block_parallel(cg, g_engine.blockSize);
        memcpy(outNew, cg->edgeBufs[cg->masterEdge], g_engine.blockSize*sizeof(float));
        printf("v0 block %d: out[0]=%.3f\n", b, outNew[0]);
    }

    // Send a live parameter change to gain A (logical_id 0x3333)
    ParamMsg pm = { .kind=PARAM_SET_GAIN, .logical_id=0x3333, .fvalue=0.2f };
    params_push(g0->params, pm);

    printf("\n--- Apply param change (gainA=0.2) ---\n");
    GraphState* cg = atomic_load(&g_engine.current);
    apply_params(cg);
    process_block_parallel(cg, g_engine.blockSize);
    memcpy(outNew, cg->edgeBufs[cg->masterEdge], g_engine.blockSize*sizeof(float));
    printf("v0 after param: out[0]=%.3f\n", outNew[0]);

    // Build new graph off-thread (here same thread for demo), migrate, then hot-swap with crossfade
    g1 = build_graph_v1(g_engine.sampleRate, g_engine.blockSize);
    migrate_state(g1, g0);

    printf("\n--- HOT SWAP -> graph v1 with crossfade %d blocks ---\n", g_engine.crossfade_len);
    atomic_store(&g_engine.prev, g0);
    atomic_store(&g_engine.current, g1);
    atomic_store(&g_engine.crossfade_blocks_left, g_engine.crossfade_len);

    for(int b=0;b<10;b++){
        GraphState* cur = atomic_load(&g_engine.current);
        GraphState* prv = atomic_load(&g_engine.prev);
        int cfleft = atomic_load(&g_engine.crossfade_blocks_left);

        // process current (parallel)
        apply_params(cur);
        process_block_parallel(cur, g_engine.blockSize);
        memcpy(outNew, cur->edgeBufs[cur->masterEdge], g_engine.blockSize*sizeof(float));

        if(prv && cfleft > 0){
            // process prev (single-thread shadow)
            process_block_single(prv, g_engine.blockSize);
            memcpy(outOld, prv->edgeBufs[prv->masterEdge], g_engine.blockSize*sizeof(float));
            float a = (float)(g_engine.crossfade_len - cfleft + 1) / (float)(g_engine.crossfade_len);
            for(int i=0;i<g_engine.blockSize;i++) mix[i] = a*outNew[i] + (1.f-a)*outOld[i];
            atomic_store(&g_engine.crossfade_blocks_left, cfleft-1);
            if(cfleft-1 == 0){
                // retire old
                atomic_store(&g_engine.prev, NULL);
                free_graph(prv);
                printf("(old graph retired)\n");
            }
        } else {
            memcpy(mix, outNew, g_engine.blockSize*sizeof(float));
        }

        printf("block %d: cur=%s cfleft=%d out[0]=%.3f\n", b, cur->label, atomic_load(&g_engine.crossfade_blocks_left), mix[0]);
    }

    engine_stop_workers();
    free_graph(g1);
    // g0 already freed after retire

    free(outNew); free(outOld); free(mix);
    printf("\nDone.\n");
    return 0;
}

// ===================== Live Editing Demo (Web Audio Style!) =====================
int main(void) {
    printf("=== Live Editing Audio Graph Demo ===\n");
    printf("This demonstrates Web Audio-style live connect/disconnect\n\n");
    
    const int block_size = 128;
    printf("Creating live graph...\n");
    LiveGraph* lg = create_live_graph(16, block_size, "live_graph");
    printf("Live graph created successfully\n");
    
    // Create some initial nodes
    int osc1 = live_add_oscillator(lg, 440.0f, "osc1_A4");    // A4
    int osc2 = live_add_oscillator(lg, 660.0f, "osc2_E5");    // E5
    int gain1 = live_add_gain(lg, 0.5f, "gain1");
    int gain2 = live_add_gain(lg, 0.3f, "gain2");
    int mixer = live_add_mixer2(lg, "mixer");
    
    float* output_buffer = calloc(block_size, sizeof(float));
    
    printf("\n--- Building initial graph: osc1->gain1->mixer, osc2->gain2->mixer ---\n");
    live_connect(lg, osc1, gain1);
    live_connect(lg, osc2, gain2);
    live_connect(lg, gain1, mixer);
    live_connect(lg, gain2, mixer);
    
    // Process a few blocks with initial configuration
    printf("\nProcessing with initial configuration:\n");
    for (int block = 0; block < 4; block++) {
        process_live_block(lg, block_size);
        
        // Get output from mixer (find its input edge)
        int output_node = find_live_output(lg);
        if (output_node >= 0 && lg->nodes[output_node].nInputs > 0) {
            int master_edge = lg->nodes[output_node].inEdges[0];
            memcpy(output_buffer, lg->edge_buffers[master_edge], block_size * sizeof(float));
            printf("  Block %d: output[0] = %.3f\n", block, output_buffer[0]);
        }
    }
    
    printf("\n--- LIVE EDIT: Disconnect osc1 from gain1, connect directly to mixer ---\n");
    live_disconnect(lg, osc1, gain1);  // Remove osc1->gain1
    live_connect(lg, osc1, mixer);     // Add osc1->mixer (bypassing gain)
    
    printf("Processing after live reconnection:\n");
    for (int block = 0; block < 4; block++) {
        process_live_block(lg, block_size);
        
        int output_node = find_live_output(lg);
        if (output_node >= 0 && lg->nodes[output_node].nInputs > 0) {
            int master_edge = lg->nodes[output_node].inEdges[0];
            memcpy(output_buffer, lg->edge_buffers[master_edge], block_size * sizeof(float));
            printf("  Block %d: output[0] = %.3f (osc1 now louder!)\n", block, output_buffer[0]);
        }
    }
    
    printf("\n--- LIVE EDIT: Add a third oscillator on the fly ---\n");
    int osc3 = live_add_oscillator(lg, 880.0f, "osc3_A5");    // A5 (octave)
    int gain3 = live_add_gain(lg, 0.2f, "gain3");
    
    live_connect(lg, osc3, gain3);
    live_connect(lg, gain3, mixer);
    
    printf("Processing with new oscillator added live:\n");
    for (int block = 0; block < 4; block++) {
        process_live_block(lg, block_size);
        
        int output_node = find_live_output(lg);
        if (output_node >= 0 && lg->nodes[output_node].nInputs > 0) {
            int master_edge = lg->nodes[output_node].inEdges[0];
            memcpy(output_buffer, lg->edge_buffers[master_edge], block_size * sizeof(float));
            printf("  Block %d: output[0] = %.3f (now with 3 oscillators!)\n", block, output_buffer[0]);
        }
    }
    
    printf("\n--- LIVE EDIT: Disconnect all and rebuild different topology ---\n");
    live_disconnect(lg, osc1, mixer);
    live_disconnect(lg, osc2, gain2);
    live_disconnect(lg, gain2, mixer);
    live_disconnect(lg, gain3, mixer);
    
    // New topology: chain them! osc1->gain1->osc2(used as gain)->gain2->mixer
    // (This is silly but demonstrates arbitrary reconnections)
    live_connect(lg, gain1, gain2);
    live_connect(lg, gain2, mixer);
    
    printf("Processing with chain topology (osc1->gain1->gain2->mixer, osc3->gain3->mixer):\n");
    for (int block = 0; block < 4; block++) {
        process_live_block(lg, block_size);
        
        int output_node = find_live_output(lg);
        if (output_node >= 0 && lg->nodes[output_node].nInputs > 0) {
            int master_edge = lg->nodes[output_node].inEdges[0];
            memcpy(output_buffer, lg->edge_buffers[master_edge], block_size * sizeof(float));
            printf("  Block %d: output[0] = %.3f (chain topology)\n", block, output_buffer[0]);
        }
    }
    
    printf("\n=== Live editing demo complete! ===\n");
    printf("All connections were changed while 'audio' was running - no compilation needed!\n");
    
    free(output_buffer);
    // Note: Should add proper cleanup for LiveGraph, but this is a demo
    return 0;
}
