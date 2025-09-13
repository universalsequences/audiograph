#include "graph_edit.h"
#include "graph_engine.h"
#include "graph_nodes.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

// Test SUM→SUM chains to see if our unique predecessor logic handles them
int main() {
  printf("=== Testing SUM→SUM Chain Scenario ===\n");

  initialize_engine(64, 48000);
  LiveGraph *lg = create_live_graph(16, 128, "sum_chain_test");
  assert(lg);

  // Create multiple NUMBER nodes
  int num1 = live_add_number(lg, 10.0f, "num1");
  int num2 = live_add_number(lg, 20.0f, "num2");
  int num3 = live_add_number(lg, 5.0f, "num3");
  int num4 = live_add_number(lg, 15.0f, "num4");
  assert(num1 >= 0 && num2 >= 0 && num3 >= 0 && num4 >= 0);

  // Create two intermediate GAIN nodes
  int gain1 = live_add_gain(lg, 1.0f, "gain1");
  int gain2 = live_add_gain(lg, 1.0f, "gain2");
  assert(gain1 >= 0 && gain2 >= 0);

  apply_graph_edits(lg->graphEditQueue, lg);

  // Connect gain2 to DAC first
  bool connect_dac = apply_connect(lg, gain2, 0, lg->dac_node_id, 0);
  assert(connect_dac);

  // Create first SUM: num1,num2 → gain1 (creates SUM1)
  bool c1 = apply_connect(lg, num1, 0, gain1, 0);
  bool c2 = apply_connect(lg, num2, 0, gain1, 0); // Creates SUM1
  assert(c1 && c2);

  int sum1_id = lg->nodes[gain1].fanin_sum_node_id[0];
  assert(sum1_id != -1);
  printf("✓ Created SUM1 (id=%d) with num1,num2 inputs\n", sum1_id);

  // Create second SUM: num3,num4 → gain2 (creates SUM2)
  bool c3 = apply_connect(lg, num3, 0, gain2, 0);
  bool c4 = apply_connect(lg, num4, 0, gain2, 0); // Creates SUM2
  assert(c3 && c4);

  int sum2_id = lg->nodes[gain2].fanin_sum_node_id[0];
  assert(sum2_id != -1);
  printf("✓ Created SUM2 (id=%d) with num3,num4 inputs\n", sum2_id);

  // Now the critical test: Connect gain1 to gain2
  // This means SUM1→gain1→gain2←SUM2 (gain2 will have 2 predecessors: gain1 and
  // SUM2)
  bool c5 = apply_connect(lg, gain1, 0, gain2, 0); // Should create SUM3!
  assert(c5);

  int sum3_id = lg->nodes[gain2].fanin_sum_node_id[0];
  if (sum3_id != -1) {
    printf("✓ Created SUM3 (id=%d) combining gain1 and SUM2 inputs\n", sum3_id);
    printf("  SUM3 indegree: %d (should be 2: from gain1 and from SUM2)\n",
           lg->indegree[sum3_id]);
    printf("  gain2 indegree: %d (should be 1: from SUM3)\n",
           lg->indegree[gain2]);
    printf("  gain1 indegree: %d (should be 1: from SUM1)\n",
           lg->indegree[gain1]);
  } else {
    printf("✗ Expected SUM3 creation but didn't happen\n");
  }

  // Process and check for correct behavior
  float output_buffer[128];
  process_next_block(lg, output_buffer, 128);

  // Expected: (10+20) + (5+15) = 30 + 20 = 50
  float expected = 50.0f;
  printf("Output: %.1f (expected %.1f)\n", output_buffer[0], expected);

  if (fabsf(output_buffer[0] - expected) < 0.001f) {
    printf("✅ SUM→SUM chain works correctly!\n");
  } else {
    printf("❌ SUM→SUM chain has issues\n");
  }

  destroy_live_graph(lg);
  printf("=== SUM→SUM Chain Test Complete ===\n");
  return 0;
}
