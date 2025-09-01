#include "graph_engine.h"
#include "graph_nodes.h"
#include <assert.h>
#include <stdio.h>

void test_auto_sum() {
  printf("=== Testing Auto-SUM Feature ===\n");
  
  LiveGraph *lg = create_live_graph(32, 64, "auto_sum_test");
  assert(lg != NULL);
  
  // Create test nodes: 3 oscillators and 1 gain
  int osc1 = apply_add_node(lg, OSC_VTABLE, NULL, 1, "osc1", 0, 1);
  int osc2 = apply_add_node(lg, OSC_VTABLE, NULL, 2, "osc2", 0, 1);
  int osc3 = apply_add_node(lg, OSC_VTABLE, NULL, 3, "osc3", 0, 1);
  int gain = apply_add_node(lg, GAIN_VTABLE, NULL, 4, "gain", 1, 1);
  
  assert(osc1 >= 0 && osc2 >= 0 && osc3 >= 0 && gain >= 0);
  printf("✓ Created test nodes: osc1=%d, osc2=%d, osc3=%d, gain=%d\n", 
         osc1, osc2, osc3, gain);
  
  // Test Case 1: First connection (normal 1:1)
  bool connect1 = apply_connect(lg, osc1, 0, gain, 0);
  assert(connect1);
  printf("✓ First connection: osc1 -> gain (normal 1:1)\n");
  
  // Verify no SUM node created yet
  assert(lg->nodes[gain].fanin_sum_node_id[0] == -1);
  assert(lg->nodes[gain].inEdgeId[0] >= 0);
  printf("✓ No SUM node created for single connection\n");
  
  // Test Case 2: Second connection (should create SUM)
  bool connect2 = apply_connect(lg, osc2, 0, gain, 0);
  assert(connect2);
  printf("✓ Second connection: osc2 -> gain (should create SUM)\n");
  
  // Verify SUM node was created
  int sum_id = lg->nodes[gain].fanin_sum_node_id[0];
  assert(sum_id >= 0);
  RTNode *sum_node = &lg->nodes[sum_id];
  assert(sum_node->nInputs == 2);
  assert(sum_node->nOutputs == 1);
  assert(sum_node->vtable.process == sum_process);
  printf("✓ SUM node created with ID=%d, 2 inputs, 1 output\n", sum_id);
  
  // Verify rewiring: both oscillators should feed the SUM
  assert(sum_node->inEdgeId[0] >= 0);
  assert(sum_node->inEdgeId[1] >= 0);
  // And SUM should feed the gain
  assert(lg->nodes[gain].inEdgeId[0] == sum_node->outEdgeId[0]);
  printf("✓ Verified SUM rewiring: osc1,osc2 -> SUM -> gain\n");
  
  // Test Case 3: Third connection (should grow SUM)
  bool connect3 = apply_connect(lg, osc3, 0, gain, 0);
  assert(connect3);
  printf("✓ Third connection: osc3 -> gain (should grow SUM)\n");
  
  // Verify SUM grown to 3 inputs
  assert(lg->nodes[gain].fanin_sum_node_id[0] == sum_id); // Same SUM
  assert(sum_node->nInputs == 3);
  assert(sum_node->inEdgeId[2] >= 0);
  printf("✓ SUM grown to 3 inputs\n");
  
  // Test disconnection: remove osc2
  bool disconnect2 = apply_disconnect(lg, osc2, 0, gain, 0);
  assert(disconnect2);
  printf("✓ Disconnected osc2 from gain\n");
  
  // Verify SUM shrunk to 2 inputs
  assert(sum_node->nInputs == 2);
  printf("✓ SUM shrunk to 2 inputs\n");
  
  // Test disconnection: remove osc3 (should collapse SUM to direct connection)
  bool disconnect3 = apply_disconnect(lg, osc3, 0, gain, 0);
  assert(disconnect3);
  printf("✓ Disconnected osc3 from gain\n");
  
  // Verify SUM collapsed back to direct connection
  assert(lg->nodes[gain].fanin_sum_node_id[0] == -1);
  assert(lg->nodes[gain].inEdgeId[0] >= 0); // Direct connection exists
  printf("✓ SUM collapsed back to direct connection\n");
  
  
  // Test final disconnection
  bool disconnect1 = apply_disconnect(lg, osc1, 0, gain, 0);
  assert(disconnect1);
  printf("✓ Disconnected osc1 from gain\n");
  
  // Verify complete disconnection
  assert(lg->nodes[gain].inEdgeId[0] == -1);
  printf("✓ Complete disconnection verified\n");
  
  destroy_live_graph(lg);
  printf("=== Auto-SUM Test Completed Successfully ===\n\n");
}

int main() {
  test_auto_sum();
  return 0;
}