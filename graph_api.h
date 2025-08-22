#ifndef GRAPH_API_H
#define GRAPH_API_H

#include "graph_types.h"
#include "graph_engine.h"

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

// ===================== Graph Builder Operations =====================

GraphBuilder* create_graph_builder(void);
void add_node_to_builder(GraphBuilder* gb, AudioNode* node);
int find_node_index(GraphBuilder* gb, AudioNode* target);
void free_graph_builder(GraphBuilder* gb);

// ===================== Web Audio-Style Node Creation =====================

AudioNode* create_oscillator(GraphBuilder* gb, float freq_hz, const char* name);
AudioNode* create_gain(GraphBuilder* gb, float gain_value, const char* name);
AudioNode* create_mixer2(GraphBuilder* gb, const char* name);
AudioNode* create_mixer3(GraphBuilder* gb, const char* name);

// ===================== Connection API =====================

void connect(AudioNode* source, AudioNode* dest);

// ===================== Graph Compilation =====================

GraphState* compile_graph(GraphBuilder* gb, int sample_rate, int block_size, const char* label);
int count_total_edges(GraphBuilder* gb);
AudioNode* find_output_node(GraphBuilder* gb);


#endif // GRAPH_API_H