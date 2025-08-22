#include "graph_api.h"
#include "graph_nodes.h"

// ===================== Global State =====================

static uint64_t g_next_node_id = 0x1000;

// ===================== Graph Builder Operations =====================

GraphBuilder* create_graph_builder(void) {
    GraphBuilder* gb = calloc(1, sizeof(GraphBuilder));
    gb->node_capacity = 16;
    gb->nodes = calloc(gb->node_capacity, sizeof(AudioNode*));
    return gb;
}

void add_node_to_builder(GraphBuilder* gb, AudioNode* node) {
    if (gb->node_count >= gb->node_capacity) {
        gb->node_capacity *= 2;
        gb->nodes = realloc(gb->nodes, gb->node_capacity * sizeof(AudioNode*));
    }
    gb->nodes[gb->node_count++] = node;
}

int find_node_index(GraphBuilder* gb, AudioNode* target) {
    for (int i = 0; i < gb->node_count; i++) {
        if (gb->nodes[i] == target) return i;
    }
    return -1; // not found
}

void free_graph_builder(GraphBuilder* gb) {
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

// ===================== Web Audio-Style Node Creation =====================

AudioNode* create_oscillator(GraphBuilder* gb, float freq_hz, const char* name) {
    AudioNode* node = calloc(1, sizeof(AudioNode));
    node->logical_id = ++g_next_node_id;
    node->vtable = OSC_VTABLE;
    node->name = name;
    
    // Allocate and initialize memory
    float* memory = calloc(OSC_MEMORY_SIZE, sizeof(float));
    memory[OSC_PHASE] = 0.0f;
    memory[OSC_INC] = freq_hz / 48000.0f; // assume 48kHz for now
    node->state = memory;
    
    add_node_to_builder(gb, node);
    return node;
}

AudioNode* create_gain(GraphBuilder* gb, float gain_value, const char* name) {
    AudioNode* node = calloc(1, sizeof(AudioNode));
    node->logical_id = ++g_next_node_id;
    node->vtable = GAIN_VTABLE;
    node->name = name;
    
    // Allocate and initialize memory
    float* memory = calloc(GAIN_MEMORY_SIZE, sizeof(float));
    memory[GAIN_VALUE] = gain_value;
    node->state = memory;
    
    add_node_to_builder(gb, node);
    return node;
}

AudioNode* create_mixer2(GraphBuilder* gb, const char* name) {
    AudioNode* node = calloc(1, sizeof(AudioNode));
    node->logical_id = ++g_next_node_id;
    node->vtable = MIX2_VTABLE;
    node->name = name;
    node->state = NULL; // mixer has no memory
    
    add_node_to_builder(gb, node);
    return node;
}

AudioNode* create_mixer3(GraphBuilder* gb, const char* name) {
    AudioNode* node = calloc(1, sizeof(AudioNode));
    node->logical_id = ++g_next_node_id;
    node->vtable = (NodeVTable){ 
        .process = mix3_process, 
        .init = NULL, 
        .reset = NULL, 
        .migrate = NULL 
    };
    node->name = name;
    node->state = NULL; // mixer has no memory
    
    add_node_to_builder(gb, node);
    return node;
}

// ===================== Generic Node Creation Helper =====================

AudioNode* create_generic_node(GraphBuilder* gb, 
                               KernelFn process_fn,
                               int memory_size,
                               int num_inputs,
                               int num_outputs,
                               const char* name) {
    AudioNode* node = calloc(1, sizeof(AudioNode));
    if (!node) return NULL;
    
    node->logical_id = ++g_next_node_id;
    node->vtable.process = process_fn;
    node->vtable.init = NULL;      // Generic nodes don't have init by default
    node->vtable.reset = NULL;
    node->vtable.migrate = NULL;   // Generic nodes don't migrate by default
    node->name = name;
    
    // Allocate memory if needed
    if (memory_size > 0) {
        node->state = calloc(memory_size, sizeof(float));
        if (!node->state) {
            free(node);
            return NULL;
        }
    } else {
        node->state = NULL;
    }
    
    // Store I/O counts for potential future use
    // (Currently AudioNode doesn't store these, but RTNode will get them during compilation)
    (void)num_inputs;
    (void)num_outputs;
    
    add_node_to_builder(gb, node);
    return node;
}

// ===================== Connection API =====================

void connect(AudioNode* source, AudioNode* dest) {
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

// ===================== Graph Compilation Helpers =====================

int count_total_edges(GraphBuilder* gb) {
    int total = 0;
    for (int i = 0; i < gb->node_count; i++) {
        total += gb->nodes[i]->output_count;
    }
    return total;
}

AudioNode* find_output_node(GraphBuilder* gb) {
    for (int i = 0; i < gb->node_count; i++) {
        if (gb->nodes[i]->output_count == 0) {
            return gb->nodes[i];
        }
    }
    return NULL; // no output found
}

// ===================== Graph Compilation =====================

GraphState* compile_graph(GraphBuilder* gb, int sample_rate, int block_size, const char* label) {
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

