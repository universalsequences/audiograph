#include "graph_edit.h"
#include "graph_engine.h"
#include "graph_nodes.h"
#include <assert.h>
#include <stdio.h>

/*
 * Regression test for: SUM node overwritten by queued GE_ADD_NODE.
 *
 * Bug scenario:
 *   1. Create a graph with capacity C, fill slots 0..(C-2) immediately.
 *   2. Queue an add_node() — pre-allocates ID (C-1) but doesn't populate
 *      the slot yet (it's zeroed from calloc, sitting in the queue).
 *   3. Immediately create another node via live_add_*(), which gets ID C,
 *      triggers grow_node_capacity, and bumps node_count past (C-1).
 *   4. Immediately apply_connect() a second source to a port that already
 *      has a producer → the engine creates a hidden SUM node.  The old
 *      free-slot scan finds slot (C-1) "empty" (zeroed by calloc) and
 *      places the SUM there.
 *   5. apply_graph_edits() processes GE_ADD_NODE(C-1) → memset(slot, 0)
 *      → the SUM is destroyed.
 *
 * Expected:  After the fix (always use atomic_fetch_add for SUM IDs),
 *            the SUM survives and bus->fanin_sum_node_id[0] points to a
 *            valid node.
 */
int main() {
  printf("=== SUM Slot Overwrite Regression Test ===\n");

  /* ---- Setup: capacity 5, fill slots 0-3 immediately ---- */
  LiveGraph *lg = create_live_graph(5, 128, "sum_overwrite_test", 1);
  assert(lg != NULL);
  int dac = lg->dac_node_id; /* slot 0 */
  printf("  DAC at slot %d, node_count=%d, capacity=%d\n", dac, lg->node_count,
         lg->node_capacity);

  int src1 = live_add_oscillator(lg, 440.0f, "src1"); /* slot 1 */
  int bus = live_add_gain(lg, 1.0f, "bus");            /* slot 2 */
  int src2 = live_add_oscillator(lg, 880.0f, "src2");  /* slot 3 */

  printf("  src1=%d, bus=%d, src2=%d  (node_count=%d)\n", src1, bus, src2,
         lg->node_count);
  assert(src1 == 1 && bus == 2 && src2 == 3);
  assert(lg->node_count == 4);

  /* Connect src1 → bus:port0 (immediate — first producer on that port) */
  bool c1 = apply_connect(lg, src1, 0, bus, 0);
  assert(c1);
  printf("  Connected src1 → bus:0\n");

  /* ---- Step 1: Queue add_node (pre-allocates slot 4, still zeroed) ---- */
  int queued_id =
      add_node(lg, GAIN_VTABLE, GAIN_MEMORY_SIZE * sizeof(float),
               "queued_gain", 1, 1, NULL, 0);
  printf("  Queued add_node → pre-allocated ID %d  (next_node_id is now %d)\n",
         queued_id, (int)atomic_load(&lg->next_node_id));
  assert(queued_id == 4);

  /* Verify slot 4 is still zeroed (GE_ADD_NODE hasn't run yet) */
  assert(lg->nodes[queued_id].vtable.process == NULL);
  assert(lg->nodes[queued_id].nInputs == 0);
  assert(lg->nodes[queued_id].nOutputs == 0);
  printf("  Slot %d is zeroed (add_node is still queued)\n", queued_id);

  /* ---- Step 2: Immediate live_add — triggers grow, bumps node_count ---- */
  int extra = live_add_oscillator(lg, 220.0f, "extra"); /* slot 5 */
  printf("  Immediate live_add → slot %d  (node_count=%d, capacity=%d)\n",
         extra, lg->node_count, lg->node_capacity);
  assert(extra == 5);
  assert(lg->node_count >= 6); /* node_count now includes slot 4 in scan */

  /* Slot 4 is still zeroed in the grown array (copied from old calloc) */
  assert(lg->nodes[queued_id].vtable.process == NULL);
  printf("  Slot %d still zeroed after grow (scan will see it as 'free')\n",
         queued_id);

  /* ---- Step 3: Immediate connect src2 → bus:port0 (triggers SUM) ---- */
  bool c2 = apply_connect(lg, src2, 0, bus, 0);
  assert(c2);
  printf("  Connected src2 → bus:0 (should create SUM node)\n");

  /* Verify SUM was created and bus references it */
  RTNode *bus_node = &lg->nodes[bus];
  int sum_id = bus_node->fanin_sum_node_id[0];
  printf("  bus->fanin_sum_node_id[0] = %d\n", sum_id);
  assert(sum_id >= 0); /* SUM must exist */

  RTNode *sum_node = &lg->nodes[sum_id];
  printf("  SUM node at slot %d: vtable.process=%s, nInputs=%d, nOutputs=%d\n",
         sum_id, sum_node->vtable.process ? "valid" : "NULL",
         sum_node->nInputs, sum_node->nOutputs);
  assert(sum_node->vtable.process != NULL); /* SUM must be alive */

  /* ---- Step 4: Drain the queue (GE_ADD_NODE at slot 4) ---- */
  printf("\n  Draining command queue...\n");
  bool drain_ok = apply_graph_edits(lg->graphEditQueue, lg);
  assert(drain_ok);

  /* ---- Step 5: Verify SUM survived the drain ---- */
  printf("  After drain:\n");

  /* Re-read in case the pointer moved (grow_node_capacity can realloc) */
  bus_node = &lg->nodes[bus];
  sum_id = bus_node->fanin_sum_node_id[0];
  sum_node = &lg->nodes[sum_id];

  printf("  bus->fanin_sum_node_id[0] = %d\n", sum_id);
  printf("  SUM node at slot %d: vtable.process=%s, nInputs=%d, nOutputs=%d\n",
         sum_id, sum_node->vtable.process ? "valid" : "NULL",
         sum_node->nInputs, sum_node->nOutputs);

  /* THIS IS THE CRITICAL ASSERTION:
   * With the bug, GE_ADD_NODE(4) overwrites the SUM → vtable.process == NULL.
   * With the fix, the SUM gets a fresh ID and is never overwritten. */
  assert(sum_node->vtable.process != NULL &&
         "SUM node was destroyed by queued GE_ADD_NODE!");
  assert(sum_node->nInputs >= 2 &&
         "SUM node lost its input ports!");

  /* Also verify the queued gain node exists and didn't collide */
  RTNode *gain_node = &lg->nodes[queued_id];
  assert(gain_node->vtable.process != NULL &&
         "Queued gain node should exist after drain");
  printf("  Queued gain at slot %d: vtable.process=%s  ✓\n", queued_id,
         gain_node->vtable.process ? "valid" : "NULL");

  printf("\n=== SUM Slot Overwrite Test PASSED ===\n");
  return 0;
}
