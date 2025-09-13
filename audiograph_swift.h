#ifndef AUDIOGRAPH_SWIFT_H
#define AUDIOGRAPH_SWIFT_H

// Audiograph Swift Integration Header
// Include this header in your Swift bridging header for audiograph integration

#include "graph_api.h"
#include "graph_edit.h"
#include "graph_engine.h"
#include "graph_nodes.h"
#include "graph_types.h"

// ===================== Core Engine Functions =====================

// Initialize the audio engine (call once at app startup)
void initialize_engine(int block_size, int sample_rate);

// Start/stop worker threads for parallel processing
void engine_start_workers(int workers);
void engine_stop_workers(void);

// ===================== Live Graph Management =====================

// Create and destroy live graphs
LiveGraph *create_live_graph(int initial_capacity, int block_size,
                             const char *label);
void destroy_live_graph(LiveGraph *lg);

// ===================== Node Management =====================

// Generic node creation (returns pre-allocated node ID)
int add_node(LiveGraph *lg, NodeVTable vtable, size_t state_size,
             const char *name, int nInputs, int nOutputs,
             const void *initial_state, size_t initial_state_size);

// Convenient factory functions for common node types
int live_add_oscillator(LiveGraph *lg, float freq_hz, const char *name);
int live_add_gain(LiveGraph *lg, float gain_value, const char *name);
int live_add_number(LiveGraph *lg, float value, const char *name);
int live_add_mixer2(LiveGraph *lg, const char *name);
int live_add_mixer8(LiveGraph *lg, const char *name);
int live_add_sum(LiveGraph *lg, const char *name, int nInputs);

// Node deletion
bool delete_node(LiveGraph *lg, int node_id);

// Check if a node creation failed
bool is_failed_node(LiveGraph *lg, int node_id);

// ===================== Port-based Connections =====================

// Connect specific ports between nodes
bool graph_connect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                   int dst_port);

// Disconnect specific port connections
bool graph_disconnect(LiveGraph *lg, int src_node, int src_port, int dst_node,
                      int dst_port);

bool hot_swap_node(LiveGraph *lg, int node_id, NodeVTable vt, size_t state_size,
                   int nin, int nout, bool xfade,
                   void (*migrate)(void *, void *), const void *initial_state,
                   size_t initial_state_size);

bool replace_keep_edges(LiveGraph *lg, int node_id, NodeVTable vt,
                        size_t state_size, int nin, int nout, bool xfade,
                        void (*migrate)(void *, void *), const void *initial_state,
                        size_t initial_state_size);

// ===================== Real-time Audio Processing =====================

// Process one audio block (thread-safe, real-time safe)
void process_next_block(LiveGraph *lg, float *output_buffer, int nframes);

// ===================== Parameter Updates =====================

// Thread-safe parameter updates (non-blocking)
bool params_push(ParamRing *r, ParamMsg m);

// ===================== Watch List API =====================

// Add a node to the watch list for state monitoring
bool add_node_to_watchlist(LiveGraph *lg, int node_id);

// Remove a node from the watch list
bool remove_node_from_watchlist(LiveGraph *lg, int node_id);

// Get a copy of a watched node's current state (caller must free the result)
// Returns NULL if node is not watched or doesn't exist
// If state_size is not NULL, it will be set to the size of the returned state
void *get_node_state(LiveGraph *lg, int node_id, size_t *state_size);

// ===================== Node VTables for Custom Nodes =====================

extern const NodeVTable OSC_VTABLE;
extern const NodeVTable GAIN_VTABLE;
extern const NodeVTable NUMBER_VTABLE;
extern const NodeVTable MIX2_VTABLE;
extern const NodeVTable MIX8_VTABLE;
extern const NodeVTable DAC_VTABLE;
extern const NodeVTable SUM_VTABLE;

#endif // AUDIOGRAPH_SWIFT_H
