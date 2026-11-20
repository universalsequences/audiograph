// Critical-path-first scheduling: the engine times nodes, gives each one the
// cost of the longest chain from it to the end of the graph, and starts the
// most urgent work first. These tests check the bookkeeping (priorities and
// source order follow the measured costs, through capacity growth and
// topology edits) and that the rendered audio is unchanged throughout.
#include "../graph_edit.h"
#include "../graph_engine.h"
#include "../graph_nodes.h"
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define BLOCK_SIZE 64
#define CHEAP_TRACKS 60
#define BLOCKS_TO_SETTLE 8

// Passes its input through after burning a fixed, clearly non-trivial amount
// of time (tens of microseconds against tens of nanoseconds for a gain).
static void burn_process(float *const *in, float *const *out, int nframes,
                         void *state, void *buffers) {
  (void)state;
  (void)buffers;
  volatile float sink = 0.0f;
  for (int i = 0; i < 40000; i++)
    sink = sink * 0.999f + (float)i;
  for (int i = 0; i < nframes; i++)
    out[0][i] = in[0][i];
}

static const NodeVTable BURN_VTABLE = {.process = burn_process};

static void timeout_handler(int sig) {
  (void)sig;
  fprintf(stderr, "critical path priority test timed out\n");
  exit(2);
}

// number -> `length` burn nodes -> DAC. Returns the source; fills chain[].
static int add_heavy_chain(LiveGraph *lg, int length, int *chain) {
  int src = live_add_number(lg, 1.0f, "heavy_src");
  assert(src >= 0);
  int prev = src;
  for (int i = 0; i < length; i++) {
    chain[i] = add_node(lg, BURN_VTABLE, sizeof(float), "burn", 1, 1, NULL, 0);
    assert(chain[i] >= 0);
    assert(graph_connect(lg, prev, 0, chain[i], 0));
    prev = chain[i];
  }
  assert(graph_connect(lg, prev, 0, lg->dac_node_id, 0));
  return src;
}

static void render(LiveGraph *lg, int blocks, float expected) {
  float output[BLOCK_SIZE];
  for (int b = 0; b < blocks; b++) {
    process_next_block(lg, output, BLOCK_SIZE);
    for (int i = 0; i < BLOCK_SIZE; i++) {
      if (fabsf(output[i] - expected) > 0.01f) {
        fprintf(stderr, "block %d frame %d: got %f, expected %f\n", b, i,
                output[i], expected);
        exit(1);
      }
    }
  }
}

static bool source_leads(const LiveGraph *lg, int src) {
  if (lg->sched.source_count < 2 || lg->sched.source_nodes[0] != src)
    return false;
  for (int i = 0; i < lg->sched.source_count; i++) {
    int other = lg->sched.source_nodes[i];
    if (other != src &&
        lg->sched.node_priority[src] <= lg->sched.node_priority[other])
      return false;
  }
  return true;
}

// Costs are wall-clock readings, so one preempted run of a cheap node can
// briefly outrank the heavy chain. Keep rendering until the order settles;
// only never settling is a failure.
static void render_until_source_leads(LiveGraph *lg, int src, float expected) {
  for (int attempt = 0; attempt < 50; attempt++) {
    render(lg, BLOCKS_TO_SETTLE, expected);
    if (source_leads(lg, src))
      return;
  }
  fprintf(stderr, "source %d never led; first is %d\n", src,
          lg->sched.source_nodes[0]);
  exit(1);
}

int main(void) {
  signal(SIGALRM, timeout_handler);
  alarm(30);

  initialize_engine(BLOCK_SIZE, 48000);
  engine_start_workers(3);

  // Deliberately small so the per-node cost and priority arrays have to grow.
  LiveGraph *lg = create_live_graph(8, BLOCK_SIZE, "critical_path", 1);
  assert(lg != NULL);

  for (int t = 0; t < CHEAP_TRACKS; t++) {
    int src = live_add_number(lg, 1.0f, "src");
    int gain = live_add_gain(lg, 1.0f, "gain");
    assert(src >= 0 && gain >= 0);
    assert(graph_connect(lg, src, 0, gain, 0));
    assert(graph_connect(lg, gain, 0, lg->dac_node_id, 0));
  }
  int chain_a[4];
  int src_a = add_heavy_chain(lg, 4, chain_a);

  render(lg, BLOCKS_TO_SETTLE, CHEAP_TRACKS + 1.0f);
  assert(lg->node_capacity > 8);

#if AUDIOGRAPH_ENABLE_STALL_DIAGNOSTICS
  // No serial plan in diagnostics builds, so no priorities: audio only.
  printf("diagnostics build: priority checks skipped\n");
#else
  render_until_source_leads(lg, src_a, CHEAP_TRACKS + 1.0f);
  // Every node on the chain was timed, and priority falls along it. (The
  // source itself is so cheap it may read as zero clock ticks.)
  assert(lg->sched.node_cost[chain_a[0]] > 0.0f);
  assert(lg->sched.node_priority[src_a] >= lg->sched.node_priority[chain_a[0]]);
  for (int i = 1; i < 4; i++)
    assert(lg->sched.node_priority[chain_a[i - 1]] >
           lg->sched.node_priority[chain_a[i]]);
  printf("chain of 4 leads: priority %.0f, first cheap source %.0f\n",
         lg->sched.node_priority[src_a],
         lg->sched.node_priority[lg->sched.source_nodes[1]]);

  // A longer chain added while running takes over the front.
  int chain_b[8];
  int src_b = add_heavy_chain(lg, 8, chain_b);
  render_until_source_leads(lg, src_b, CHEAP_TRACKS + 2.0f);
  assert(lg->sched.node_cost[chain_b[7]] > 0.0f);
  // Costs measured before the edit (and any array growth) were kept.
  assert(lg->sched.node_cost[chain_a[0]] > 0.0f);

  // Removing it hands the front back.
  for (int i = 0; i < 8; i++)
    assert(delete_node(lg, chain_b[i]));
  assert(delete_node(lg, src_b));
  render_until_source_leads(lg, src_a, CHEAP_TRACKS + 1.0f);
#endif

  // Long run across several measurement periods: audio stays exact.
  render(lg, 400, CHEAP_TRACKS + 1.0f);

  destroy_live_graph(lg);
  engine_stop_workers();
  alarm(0);
  printf("critical path priority test passed\n");
  return 0;
}
