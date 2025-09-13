#include "graph_engine.h"
#include <assert.h>
#include <stdio.h>

int main() {
  printf("Testing watchlist functionality...\n");

  // Create a graph
  LiveGraph *lg = create_live_graph(16, 128, "watchlist_test");
  assert(lg != NULL);

  // Add some nodes with state
  int osc_id = live_add_oscillator(lg, 440.0f, "test_osc");
  int gain_id = live_add_gain(lg, 0.5f, "test_gain");
  
  printf("Created oscillator node: %d\n", osc_id);
  printf("Created gain node: %d\n", gain_id);

  // Add nodes to watchlist
  bool result1 = add_node_to_watchlist(lg, osc_id);
  bool result2 = add_node_to_watchlist(lg, gain_id);
  
  printf("Added osc to watchlist: %s\n", result1 ? "success" : "failed");
  printf("Added gain to watchlist: %s\n", result2 ? "success" : "failed");
  
  assert(result1 == true);
  assert(result2 == true);

  // Try to add the same node again (should succeed, no duplicate)
  bool result3 = add_node_to_watchlist(lg, osc_id);
  printf("Added osc again: %s\n", result3 ? "success" : "failed");
  assert(result3 == true);

  // Connect nodes and process a block to trigger state updates
  graph_connect(lg, osc_id, 0, gain_id, 0);
  graph_connect(lg, gain_id, 0, 0, 0); // Connect to DAC
  
  float output_buffer[128];
  process_next_block(lg, output_buffer, 128);
  
  // Get node states
  size_t osc_state_size, gain_state_size;
  void *osc_state = get_node_state(lg, osc_id, &osc_state_size);
  void *gain_state = get_node_state(lg, gain_id, &gain_state_size);
  
  printf("Retrieved osc state: size=%zu, ptr=%p\n", osc_state_size, osc_state);
  printf("Retrieved gain state: size=%zu, ptr=%p\n", gain_state_size, gain_state);
  
  // Verify we got some state data
  if (osc_state) {
    printf("Oscillator state retrieved successfully\n");
    free(osc_state);
  } else {
    printf("Warning: No oscillator state retrieved\n");
  }
  
  if (gain_state) {
    printf("Gain state retrieved successfully\n");
    free(gain_state);
  } else {
    printf("Warning: No gain state retrieved\n");
  }

  // Test invalid node ID
  void *invalid_state = get_node_state(lg, 999, NULL);
  assert(invalid_state == NULL);
  printf("Invalid node ID correctly returned NULL\n");

  // Remove node from watchlist
  bool removed = remove_node_from_watchlist(lg, osc_id);
  printf("Removed osc from watchlist: %s\n", removed ? "success" : "failed");
  assert(removed == true);
  
  // Try to remove again (should fail)
  bool removed2 = remove_node_from_watchlist(lg, osc_id);
  printf("Tried to remove osc again: %s\n", removed2 ? "success" : "failed");
  assert(removed2 == false);

  // Clean up
  destroy_live_graph(lg);
  
  printf("Watchlist test completed successfully!\n");
  return 0;
}