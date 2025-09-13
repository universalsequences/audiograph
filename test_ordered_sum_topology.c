#include "graph_edit.h"
#include "graph_engine.h"
#include "graph_nodes.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

void test_ordered_sum_topology() {
  const int block_size = 256;
  LiveGraph *lg = create_live_graph(32, block_size, "complex_topology_test");
  assert(lg != NULL);

  int node1 = live_add_number(lg, 10.0f, "num1");
  int node2 = live_add_number(lg, 10.0f, "num2");

  int node3 = live_add_gain(lg, 1.0f, "node3");
  int node4 = live_add_gain(lg, 1.0f, "node4");

  apply_graph_edits(lg->graphEditQueue, lg);

  graph_connect(lg, node2, 0, node3, 0);
  graph_connect(lg, node1, 0, node3, 0);
  graph_connect(lg, node1, 0, node4, 0);
  graph_connect(lg, node3, 0, lg->dac_node_id, 0);
  graph_connect(lg, node4, 0, lg->dac_node_id, 0);

  apply_graph_edits(lg->graphEditQueue, lg);

  float output_buffer[block_size];
  process_next_block(lg, output_buffer, block_size);

  float output_value = output_buffer[0];
  printf("++++++++++++++++++++++++++ PRE DISCONNECT: Processed block, output "
         "value: %.6f\n",
         output_value);

  graph_disconnect(lg, node2, 0, node3, 0);

  apply_graph_edits(lg->graphEditQueue, lg);

  printf("in degree of node3=%d\n", lg->indegree[node3]);

  printf("\n=== COMPLETE GRAPH STATE AFTER DISCONNECT ===\n");

  // Print all active edges
  printf("\n--- EDGE TABLE ---\n");
  for (int i = 0; i < lg->edge_capacity; i++) {
    if (lg->edges[i].in_use) {
      printf("Edge[%d]: in_use=%s, src_node=%d, src_port=%d, refcount=%d, buf=%p\n",
             i, lg->edges[i].in_use ? "true" : "false",
             lg->edges[i].src_node, lg->edges[i].src_port,
             lg->edges[i].refcount, lg->edges[i].buf);
    } else {
      printf("Edge[%d]: RETIRED\n", i);
    }
  }

  // Print all nodes with their connections
  printf("\n--- NODE CONNECTIONS ---\n");
  for (int i = 0; i < lg->node_count; i++) {
    RTNode *node = &lg->nodes[i];
    if (node->vtable.process == NULL) {
      printf("Node[%d]: DELETED\n", i);
      continue;
    }

    printf("Node[%d]: nInputs=%d, nOutputs=%d\n", i, node->nInputs, node->nOutputs);

    // Print input connections
    if (node->inEdgeId && node->nInputs > 0) {
      for (int p = 0; p < node->nInputs; p++) {
        int eid = node->inEdgeId[p];
        if (eid >= 0) {
          printf("  Input[%d] <- Edge[%d] (from Node[%d]:Port[%d])\n",
                 p, eid, lg->edges[eid].src_node, lg->edges[eid].src_port);
        } else {
          printf("  Input[%d] <- DISCONNECTED\n", p);
        }
      }
    }

    // Print output connections
    if (node->outEdgeId && node->nOutputs > 0) {
      for (int p = 0; p < node->nOutputs; p++) {
        int eid = node->outEdgeId[p];
        if (eid >= 0) {
          printf("  Output[%d] -> Edge[%d] (refcount=%d)\n",
                 p, eid, lg->edges[eid].refcount);
        } else {
          printf("  Output[%d] -> DISCONNECTED\n", p);
        }
      }
    }

    // Print SUM node fanin if applicable
    if (node->fanin_sum_node_id && node->nInputs > 0) {
      for (int p = 0; p < node->nInputs; p++) {
        int sum_id = node->fanin_sum_node_id[p];
        if (sum_id >= 0) {
          printf("  Input[%d] has SUM node: %d\n", p, sum_id);
        }
      }
    }
  }

  printf("==============================================\n\n");

  // Process a block
  process_next_block(lg, output_buffer, block_size);

  output_value = output_buffer[0];
  printf("Processed block, output value: %.6f\n", output_value);
}

int main() {
  test_ordered_sum_topology();

  return 0;
}
