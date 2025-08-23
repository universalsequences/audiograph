#include "graph_api.h"
#include "graph_engine.h"
#include "graph_types.h"

// ===================== Demo Graph Variants =====================

// ===================== Original Demo (Compiled Graphs with Hot-Swap)
// =====================

// ===================== Live Editing Demo (Web Audio Style!)
// =====================

int main_live_demo(void) {
  printf("=== Live Editing Audio Graph Demo ===\n");
  printf("This demonstrates Web Audio-style live connect/disconnect\n\n");

  const int block_size = 128;
  printf("Creating live graph...\n");
  LiveGraph *lg = create_live_graph(16, block_size, "live_graph");
  printf("Live graph created successfully\n");

  // Create some initial nodes
  int osc1 = live_add_oscillator(lg, 440.0f, "osc1_A4"); // A4
  int osc2 = live_add_oscillator(lg, 660.0f, "osc2_E5"); // E5
  int gain1 = live_add_gain(lg, 0.5f, "gain1");
  int gain2 = live_add_gain(lg, 0.3f, "gain2");
  int mixer = live_add_mixer2(lg, "mixer");

  float *output_buffer = calloc(block_size, sizeof(float));

  printf("\n--- Building initial graph: osc1->gain1->mixer, osc2->gain2->mixer "
         "---\n");
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
      memcpy(output_buffer, lg->edge_buffers[master_edge],
             block_size * sizeof(float));
      printf("  Block %d: output[0] = %.3f\n", block, output_buffer[0]);
    }
  }

  printf("\n--- LIVE EDIT: Disconnect osc1 from gain1, connect directly to "
         "mixer ---\n");
  live_disconnect(lg, osc1, gain1); // Remove osc1->gain1
  live_connect(lg, osc1, mixer);    // Add osc1->mixer (bypassing gain)

  printf("Processing after live reconnection:\n");
  for (int block = 0; block < 4; block++) {
    process_live_block(lg, block_size);

    int output_node = find_live_output(lg);
    if (output_node >= 0 && lg->nodes[output_node].nInputs > 0) {
      int master_edge = lg->nodes[output_node].inEdges[0];
      memcpy(output_buffer, lg->edge_buffers[master_edge],
             block_size * sizeof(float));
      printf("  Block %d: output[0] = %.3f (osc1 now louder!)\n", block,
             output_buffer[0]);
    }
  }

  printf("\n--- LIVE EDIT: Add a third oscillator on the fly ---\n");
  int osc3 = live_add_oscillator(lg, 880.0f, "osc3_A5"); // A5 (octave)
  int gain3 = live_add_gain(lg, 0.2f, "gain3");

  live_connect(lg, osc3, gain3);
  live_connect(lg, gain3, mixer);

  printf("Processing with new oscillator added live:\n");
  for (int block = 0; block < 4; block++) {
    process_live_block(lg, block_size);

    int output_node = find_live_output(lg);
    if (output_node >= 0 && lg->nodes[output_node].nInputs > 0) {
      int master_edge = lg->nodes[output_node].inEdges[0];
      memcpy(output_buffer, lg->edge_buffers[master_edge],
             block_size * sizeof(float));
      printf("  Block %d: output[0] = %.3f (now with 3 oscillators!)\n", block,
             output_buffer[0]);
    }
  }

  printf(
      "\n--- LIVE EDIT: Disconnect all and rebuild different topology ---\n");
  live_disconnect(lg, osc1, mixer);
  live_disconnect(lg, osc2, gain2);
  live_disconnect(lg, gain2, mixer);
  live_disconnect(lg, gain3, mixer);

  // New topology: chain them! osc1->gain1->gain2->mixer
  // (This is silly but demonstrates arbitrary reconnections)
  live_connect(lg, gain1, gain2);
  live_connect(lg, gain2, mixer);

  printf("Processing with chain topology (osc1->gain1->gain2->mixer, "
         "osc3->gain3->mixer):\n");
  for (int block = 0; block < 4; block++) {
    process_live_block(lg, block_size);

    int output_node = find_live_output(lg);
    if (output_node >= 0 && lg->nodes[output_node].nInputs > 0) {
      int master_edge = lg->nodes[output_node].inEdges[0];
      memcpy(output_buffer, lg->edge_buffers[master_edge],
             block_size * sizeof(float));
      printf("  Block %d: output[0] = %.3f (chain topology)\n", block,
             output_buffer[0]);
    }
  }

  printf("\n=== Live editing demo complete! ===\n");
  printf("All connections were changed while 'audio' was running - no "
         "compilation needed!\n");

  free(output_buffer);
  // Note: Should add proper cleanup for LiveGraph, but this is a demo
  return 0;
}

// ===================== Main Entry Point =====================

int main(int argc, char *argv[]) {
  // Default to live demo, allow choosing compiled demo
  printf("Running live editing demo (use './audiograph compiled' for "
         "compiled demo)...\n\n");
  return main_live_demo();
}
