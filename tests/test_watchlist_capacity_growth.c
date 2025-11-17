#include "graph_api.h"
#include "graph_engine.h"
#include "graph_nodes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const int initial_capacity = 4;
  const int block_size = 128;
  LiveGraph *lg = create_live_graph(initial_capacity, block_size,
                                    "watchlist_capacity_growth", 1);
  if (!lg) {
    printf("FAILED: create_live_graph returned NULL\n");
    return 1;
  }

  void **initial_snapshots = lg->state_snapshots;
  size_t *initial_sizes = lg->state_sizes;
  const int total_nodes = 150; // ensure multiple growth operations

  for (int i = 1; i <= total_nodes; i++) {
    float init = (float)i;
    int node_id = apply_add_node(
        lg, NUMBER_VTABLE, NUMBER_MEMORY_SIZE * sizeof(float), (uint64_t)i,
        "number", 0, 1, &init);
    if (node_id < 0) {
      printf("FAILED: apply_add_node returned %d at iteration %d\n", node_id,
             i);
      destroy_live_graph(lg);
      return 1;
    }

    if (i % 30 == 0) {
      if (!add_node_to_watchlist(lg, node_id)) {
        printf("FAILED: add_node_to_watchlist for node %d\n", node_id);
        destroy_live_graph(lg);
        return 1;
      }
    }
  }

  if (lg->node_capacity <= initial_capacity) {
    printf("FAILED: node capacity did not grow (still %d)\n",
           lg->node_capacity);
    destroy_live_graph(lg);
    return 1;
  }

  if (lg->state_snapshots == initial_snapshots) {
    printf("FAILED: state_snapshots pointer did not change after growth\n");
    destroy_live_graph(lg);
    return 1;
  }

  if (lg->state_sizes == initial_sizes) {
    printf("FAILED: state_sizes pointer did not change after growth\n");
    destroy_live_graph(lg);
    return 1;
  }

  const int watched_nodes[] = {30, 60, 90, 120, 150};
  const size_t expected_size = NUMBER_MEMORY_SIZE * sizeof(float);

  // Confirm newly sized arrays are addressable for high node IDs.
  for (size_t i = 0; i < sizeof(watched_nodes) / sizeof(watched_nodes[0]);
       i++) {
    int node_id = watched_nodes[i];
    if (node_id >= lg->node_capacity) {
      printf("FAILED: node_id %d outside node_capacity %d\n", node_id,
             lg->node_capacity);
      destroy_live_graph(lg);
      return 1;
    }
    if (lg->state_snapshots[node_id] != NULL ||
        lg->state_sizes[node_id] != 0) {
      printf("FAILED: expected fresh slots at node %d to be zeroed\n", node_id);
      destroy_live_graph(lg);
      return 1;
    }

    // Simulate snapshot allocation to ensure write/read succeeds.
    lg->state_snapshots[node_id] = malloc(expected_size);
    if (!lg->state_snapshots[node_id]) {
      printf("FAILED: malloc for node %d snapshot slot\n", node_id);
      destroy_live_graph(lg);
      return 1;
    }
    lg->state_sizes[node_id] = expected_size;
    memset(lg->state_snapshots[node_id], 0xAB, expected_size);
  }

  // Ensure runtime functions can run without crashing after growth.
  process_live_block(lg, block_size);

  // Clean up allocated test buffers.
  for (size_t i = 0; i < sizeof(watched_nodes) / sizeof(watched_nodes[0]);
       i++) {
    int node_id = watched_nodes[i];
    free(lg->state_snapshots[node_id]);
    lg->state_snapshots[node_id] = NULL;
    lg->state_sizes[node_id] = 0;
  }

  destroy_live_graph(lg);
  printf("SUCCESS: state snapshot arrays resized with node capacity growth\n");
  return 0;
}
