#include "graph_api.h"
#include "graph_engine.h"
#include "graph_types.h"

// ===================== Demo Graph Variants =====================

static GraphState *build_graph_v0(int sr, int block) {
  GraphBuilder *gb = create_graph_builder();

  // Create nodes with nice names and frequencies
  AudioNode *oscA = create_oscillator(gb, 440.0f, "oscA"); // A4 = 440 Hz
  AudioNode *oscB =
      create_oscillator(gb, 660.0f, "oscB"); // E5 = 660 Hz (perfect fifth)
  AudioNode *gainA = create_gain(gb, 0.5f, "gainA");
  AudioNode *gainB = create_gain(gb, 0.8f, "gainB");
  AudioNode *mixer = create_mixer2(gb, "mixer");

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
  GraphState *g = compile_graph(gb, sr, block, "graph_v0");

  // Clean up the builder (nodes transferred to compiled graph)
  free_graph_builder(gb);
  return g;
}

static GraphState *build_graph_v1(int sr, int block) {
  GraphBuilder *gb = create_graph_builder();

  // Create nodes with different frequencies for a richer chord
  AudioNode *oscA = create_oscillator(gb, 440.0f, "oscA"); // A4 = 440 Hz
  AudioNode *oscB = create_oscillator(gb, 660.0f, "oscB"); // E5 = 660 Hz
  AudioNode *oscC =
      create_oscillator(gb, 880.0f, "oscC");         // A5 = 880 Hz (octave)
  AudioNode *gainA = create_gain(gb, 0.7f, "gainA"); // slightly different gains
  AudioNode *gainB = create_gain(gb, 0.6f, "gainB");
  AudioNode *gainC = create_gain(gb, 0.5f, "gainC");
  AudioNode *mixer = create_mixer3(gb, "mixer3"); // 3-input mixer

  // Connect the graph: three oscillator chains feeding into one mixer
  connect(oscA, gainA);
  connect(oscB, gainB);
  connect(oscC, gainC);
  connect(gainA, mixer);
  connect(gainB, mixer);
  connect(gainC, mixer);

  // Set specific logical IDs to match the old system (for parameter updates and
  // migration)
  oscA->logical_id = 0x1111;
  oscB->logical_id = 0x2222;
  oscC->logical_id = 0x6666;
  gainA->logical_id = 0x3333;
  gainB->logical_id = 0x4444;
  gainC->logical_id = 0x7777;
  mixer->logical_id = 0x5555;

  // Compile to runtime format
  GraphState *g = compile_graph(gb, sr, block, "graph_v1");

  // Clean up the builder
  free_graph_builder(gb);
  return g;
}

// ===================== Original Demo (Compiled Graphs with Hot-Swap)
// =====================

int main_compiled_demo(void) {
  g_engine.sampleRate = 48000;
  g_engine.blockSize = 128;
  g_engine.crossfade_len = 4;

  GraphState *g0 = build_graph_v0(g_engine.sampleRate, g_engine.blockSize);
  GraphState *g1 = NULL;

  atomic_store(&g_engine.current, g0);
  atomic_store(&g_engine.prev, NULL);
  atomic_store(&g_engine.workSession, NULL);
  atomic_store(&g_engine.crossfade_blocks_left, 0);

  int cpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
  int workers = cpu > 1 ? cpu - 1 : 1;
  engine_start_workers(workers);

  float *outNew = (float *)calloc(g_engine.blockSize, sizeof(float));
  float *outOld = (float *)calloc(g_engine.blockSize, sizeof(float));
  float *mix = (float *)calloc(g_engine.blockSize, sizeof(float));

  printf("\n--- Running initial graph v0 for a few blocks ---\n");
  for (int b = 0; b < 6; b++) {
    GraphState *cg = atomic_load(&g_engine.current);
    apply_params(cg);
    process_block_parallel(cg, g_engine.blockSize);
    memcpy(outNew, cg->edgeBufs[cg->masterEdge],
           g_engine.blockSize * sizeof(float));
    printf("v0 block %d: out[0]=%.3f\n", b, outNew[0]);
  }

  // Send a live parameter change to gain A (logical_id 0x3333)
  ParamMsg pm = {.idx = 1, .logical_id = 0x3333, .fvalue = 0.2f};
  params_push(g0->params, pm);

  printf("\n--- Apply param change (gainA=0.2) ---\n");
  GraphState *cg = atomic_load(&g_engine.current);
  apply_params(cg);
  process_block_parallel(cg, g_engine.blockSize);
  memcpy(outNew, cg->edgeBufs[cg->masterEdge],
         g_engine.blockSize * sizeof(float));
  printf("v0 after param: out[0]=%.3f\n", outNew[0]);

  // Build new graph off-thread (here same thread for demo), migrate, then
  // hot-swap with crossfade
  g1 = build_graph_v1(g_engine.sampleRate, g_engine.blockSize);
  migrate_state(g1, g0);

  printf("\n--- HOT SWAP -> graph v1 with crossfade %d blocks ---\n",
         g_engine.crossfade_len);
  atomic_store(&g_engine.prev, g0);
  atomic_store(&g_engine.current, g1);
  atomic_store(&g_engine.crossfade_blocks_left, g_engine.crossfade_len);

  for (int b = 0; b < 10; b++) {
    GraphState *cur = atomic_load(&g_engine.current);
    GraphState *prv = atomic_load(&g_engine.prev);
    int cfleft = atomic_load(&g_engine.crossfade_blocks_left);

    // process current (parallel)
    apply_params(cur);
    process_block_parallel(cur, g_engine.blockSize);
    memcpy(outNew, cur->edgeBufs[cur->masterEdge],
           g_engine.blockSize * sizeof(float));

    if (prv && cfleft > 0) {
      // process prev (single-thread shadow)
      process_block_single(prv, g_engine.blockSize);
      memcpy(outOld, prv->edgeBufs[prv->masterEdge],
             g_engine.blockSize * sizeof(float));
      float a = (float)(g_engine.crossfade_len - cfleft + 1) /
                (float)(g_engine.crossfade_len);
      for (int i = 0; i < g_engine.blockSize; i++) {
        mix[i] = a * outNew[i] + (1.f - a) * outOld[i];
      }
      atomic_store(&g_engine.crossfade_blocks_left, cfleft - 1);
      if (cfleft - 1 == 0) {
        // retire old
        atomic_store(&g_engine.prev, NULL);
        free_graph(prv);
        printf("(old graph retired)\n");
      }
    } else {
      memcpy(mix, outNew, g_engine.blockSize * sizeof(float));
    }

    printf("block %d: cur=%s cfleft=%d out[0]=%.3f\n", b, cur->label,
           atomic_load(&g_engine.crossfade_blocks_left), mix[0]);
  }

  engine_stop_workers();
  free_graph(g1);
  // g0 already freed after retire

  free(outNew);
  free(outOld);
  free(mix);
  printf("\nDone.\n");
  return 0;
}

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
  if (argc > 1 && strcmp(argv[1], "compiled") == 0) {
    printf("Running compiled graph demo with hot-swapping...\n\n");
    return main_compiled_demo();
  } else {
    printf("Running live editing demo (use './audiograph compiled' for "
           "compiled demo)...\n\n");
    return main_live_demo();
  }
}
