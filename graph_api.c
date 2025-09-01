#include "graph_api.h"
#include "graph_nodes.h"

// ===================== Global State =====================

static uint64_t g_next_node_id = 0x1000;

// ===================== Graph Builder Operations =====================

GraphBuilder *create_graph_builder(void) {
  GraphBuilder *gb = calloc(1, sizeof(GraphBuilder));
  gb->node_capacity = 16;
  gb->nodes = calloc(gb->node_capacity, sizeof(AudioNode *));
  return gb;
}

void add_node_to_builder(GraphBuilder *gb, AudioNode *node) {
  if (gb->node_count >= gb->node_capacity) {
    gb->node_capacity *= 2;
    gb->nodes = realloc(gb->nodes, gb->node_capacity * sizeof(AudioNode *));
  }
  gb->nodes[gb->node_count++] = node;
}

int find_node_index(GraphBuilder *gb, AudioNode *target) {
  for (int i = 0; i < gb->node_count; i++) {
    if (gb->nodes[i] == target)
      return i;
  }
  return -1; // not found
}

void free_graph_builder(GraphBuilder *gb) {
  for (int i = 0; i < gb->node_count; i++) {
    AudioNode *node = gb->nodes[i];
    free(node->inputs);
    free(node->outputs);
    // Note: don't free node->state - it's transferred to the compiled graph
    free(node);
  }
  free(gb->nodes);
  free(gb);
}

// ===================== Web Audio-Style Node Creation =====================

AudioNode *create_oscillator(GraphBuilder *gb, float freq_hz,
                             const char *name) {
  AudioNode *node = calloc(1, sizeof(AudioNode));
  node->logical_id = ++g_next_node_id;
  node->vtable = OSC_VTABLE;
  node->name = name;

  // Allocate and initialize memory
  float *memory = calloc(OSC_MEMORY_SIZE, sizeof(float));
  memory[OSC_PHASE] = 0.0f;
  memory[OSC_INC] = freq_hz / 48000.0f; // assume 48kHz for now
  node->state = memory;

  add_node_to_builder(gb, node);
  return node;
}

AudioNode *create_gain(GraphBuilder *gb, float gain_value, const char *name) {
  AudioNode *node = calloc(1, sizeof(AudioNode));
  node->logical_id = ++g_next_node_id;
  node->vtable = GAIN_VTABLE;
  node->name = name;

  // Allocate and initialize memory
  float *memory = calloc(GAIN_MEMORY_SIZE, sizeof(float));
  memory[GAIN_VALUE] = gain_value;
  node->state = memory;

  add_node_to_builder(gb, node);
  return node;
}

AudioNode *create_mixer2(GraphBuilder *gb, const char *name) {
  AudioNode *node = calloc(1, sizeof(AudioNode));
  node->logical_id = ++g_next_node_id;
  node->vtable = MIX2_VTABLE;
  node->name = name;
  node->state = NULL; // mixer has no memory

  add_node_to_builder(gb, node);
  return node;
}

AudioNode *create_mixer3(GraphBuilder *gb, const char *name) {
  AudioNode *node = calloc(1, sizeof(AudioNode));
  node->logical_id = ++g_next_node_id;
  node->vtable = (NodeVTable){
      .process = mix3_process, .init = NULL, .reset = NULL, .migrate = NULL};
  node->name = name;
  node->state = NULL; // mixer has no memory

  add_node_to_builder(gb, node);
  return node;
}

// ===================== Generic Node Creation Helper =====================

AudioNode *create_generic_node(GraphBuilder *gb, KernelFn process_fn,
                               int memory_size, int num_inputs, int num_outputs,
                               const char *name) {
  AudioNode *node = calloc(1, sizeof(AudioNode));
  if (!node)
    return NULL;

  node->logical_id = ++g_next_node_id;
  node->vtable.process = process_fn;
  node->vtable.init = NULL; // Generic nodes don't have init by default
  node->vtable.reset = NULL;
  node->vtable.migrate = NULL; // Generic nodes don't migrate by default
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
  // (Currently AudioNode doesn't store these, but RTNode will get them during
  // compilation)
  (void)num_inputs;
  (void)num_outputs;

  add_node_to_builder(gb, node);
  return node;
}

// ===================== Connection API =====================

// ===================== Graph Compilation Helpers =====================

int count_total_edges(GraphBuilder *gb) {
  int total = 0;
  for (int i = 0; i < gb->node_count; i++) {
    total += gb->nodes[i]->output_count;
  }
  return total;
}
