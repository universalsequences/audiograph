#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graph_edit.h"
#include "graph_engine.h"
#include "graph_nodes.h"

// Same node definitions as stress test
typedef struct { float value; } NumberGenState;
typedef struct { float value1; float value2; } DualOutputState;
typedef struct { float dummy; } MultiplierState;

static void number_gen_process(float *const *inputs, float *const *outputs,
                              int block_size, void *state) {
    (void)inputs;
    NumberGenState *s = (NumberGenState *)state;
    for (int i = 0; i < block_size; i++) {
        outputs[0][i] = s->value;
    }
}

static void dual_output_process(float *const *inputs, float *const *outputs,
                               int block_size, void *state) {
    (void)inputs;
    DualOutputState *s = (DualOutputState *)state;
    for (int i = 0; i < block_size; i++) {
        outputs[0][i] = s->value1;
        outputs[1][i] = s->value2;
    }
}

static void multiplier_process(float *const *inputs, float *const *outputs,
                              int block_size, void *state) {
    (void)state;
    for (int i = 0; i < block_size; i++) {
        outputs[0][i] = inputs[0][i] * inputs[1][i];
    }
}

static void number_gen_init(void *state, int sr, int mb) {
    (void)sr; (void)mb;
    NumberGenState *s = (NumberGenState *)state;
    s->value = 1.0f;
}

static void dual_output_init(void *state, int sr, int mb) {
    (void)sr; (void)mb;
    DualOutputState *s = (DualOutputState *)state;
    s->value1 = 2.0f;
    s->value2 = 3.0f;
}

static void multiplier_init(void *state, int sr, int mb) {
    (void)sr; (void)mb; (void)state;
}

static const NodeVTable NUMBER_GEN_VTABLE = {
    .init = number_gen_init,
    .process = number_gen_process,
    .reset = NULL,
    .migrate = NULL
};

static const NodeVTable DUAL_OUTPUT_VTABLE = {
    .init = dual_output_init,
    .process = dual_output_process,
    .reset = NULL,
    .migrate = NULL
};

static const NodeVTable MULTIPLIER_VTABLE = {
    .init = multiplier_init,
    .process = multiplier_process,
    .reset = NULL,
    .migrate = NULL
};

static LiveGraph *g_reused_graph = NULL;
static int g_node1_id, g_node2_id, g_node3_id, g_node4_id;

static void initialize_reusable_graph() {
    if (g_reused_graph) return;
    
    g_reused_graph = create_live_graph(32, 256, "debug_iteration_124");
    assert(g_reused_graph != NULL);
    
    g_node1_id = add_node(g_reused_graph, NUMBER_GEN_VTABLE, sizeof(NumberGenState), "number_gen", 0, 1);
    g_node2_id = add_node(g_reused_graph, DUAL_OUTPUT_VTABLE, sizeof(DualOutputState), "dual_output", 0, 2);
    g_node3_id = add_node(g_reused_graph, MULTIPLIER_VTABLE, sizeof(MultiplierState), "multiplier", 2, 1);
    g_node4_id = live_add_gain(g_reused_graph, 0.5f, "gain");
    
    assert(g_node1_id >= 0 && g_node2_id >= 0 && g_node3_id >= 0 && g_node4_id >= 0);
    apply_graph_edits(g_reused_graph->graphEditQueue, g_reused_graph);
}

static void disconnect_all_edges() {
    LiveGraph *lg = g_reused_graph;
    
    graph_disconnect(lg, g_node1_id, 0, g_node3_id, 0);     // N1→N3
    graph_disconnect(lg, g_node1_id, 0, g_node4_id, 0);     // N1→N4  
    graph_disconnect(lg, g_node2_id, 0, g_node3_id, 1);     // N2→N3
    apply_disconnect(lg, g_node2_id, 1, lg->dac_node_id, 0); // N2→DAC
    graph_disconnect(lg, g_node3_id, 0, lg->dac_node_id, 0); // N3→DAC
    graph_disconnect(lg, g_node4_id, 0, lg->dac_node_id, 0); // N4→DAC
    
    apply_graph_edits(lg->graphEditQueue, lg);
}

static void reconnect_full_topology() {
    LiveGraph *lg = g_reused_graph;
    
    graph_connect(lg, g_node1_id, 0, g_node3_id, 0);     // N1→N3
    graph_connect(lg, g_node1_id, 0, g_node4_id, 0);     // N1→N4  
    graph_connect(lg, g_node2_id, 0, g_node3_id, 1);     // N2→N3
    apply_connect(lg, g_node2_id, 1, lg->dac_node_id, 0); // N2→DAC
    graph_connect(lg, g_node3_id, 0, lg->dac_node_id, 0); // N3→DAC
    graph_connect(lg, g_node4_id, 0, lg->dac_node_id, 0); // N4→DAC
    
    apply_graph_edits(lg->graphEditQueue, lg);
}

static void dump_dac_edge_state(const char* label) {
    LiveGraph *lg = g_reused_graph;
    RTNode *dac = &lg->nodes[lg->dac_node_id];
    
    printf("🔍 %s - DAC Edge Analysis:\n", label);
    printf("     DAC indegree: %d\n", lg->indegree[lg->dac_node_id]);
    printf("     DAC nInputs: %d\n", dac->nInputs);
    
    if (dac->inEdgeId) {
        for (int i = 0; i < dac->nInputs; i++) {
            int edge_id = dac->inEdgeId[i];
            if (edge_id >= 0) {
                LiveEdge *edge = &lg->edges[edge_id];
                printf("     DAC Input[%d]: Edge %d from Node %d (port %d) refcount=%d\n", 
                       i, edge_id, edge->src_node, edge->src_port, edge->refcount);
            } else {
                printf("     DAC Input[%d]: EMPTY (edge_id = %d)\n", i, edge_id);
            }
        }
    } else {
        printf("     DAC inEdgeId array: NULL\n");
    }
    
    // Check if we can find the N4→DAC connection
    RTNode *node4 = &lg->nodes[g_node4_id];
    printf("     N4 outEdgeId[0]: %d\n", node4->outEdgeId ? node4->outEdgeId[0] : -999);
    if (node4->outEdgeId && node4->outEdgeId[0] >= 0) {
        LiveEdge *n4_edge = &lg->edges[node4->outEdgeId[0]];
        printf("     N4→? Edge: src_node=%d, src_port=%d, refcount=%d\n", 
               n4_edge->src_node, n4_edge->src_port, n4_edge->refcount);
    }
}

static bool run_single_iteration_debug(int iteration) {
    LiveGraph *lg = g_reused_graph;
    
    printf("\n=== ITERATION %d DEBUG ===\n", iteration);
    
    disconnect_all_edges();
    reconnect_full_topology();
    
    dump_dac_edge_state("Before disconnect sequence");
    
    // Step 1: N1→N4 disconnect
    bool step1_ok = graph_disconnect(lg, g_node1_id, 0, g_node4_id, 0);
    if (step1_ok) apply_graph_edits(lg->graphEditQueue, lg);
    
    // Step 2: N4→DAC disconnect (this is what fails at iteration 124)
    printf("\n🎯 Attempting critical N4→DAC disconnect...\n");
    dump_dac_edge_state("Right before N4→DAC disconnect");
    
    // Manual check of apply_disconnect conditions
    RTNode *n4 = &lg->nodes[g_node4_id];
    RTNode *dac = &lg->nodes[lg->dac_node_id];
    
    printf("     Checking apply_disconnect preconditions:\n");
    printf("       N4 outEdgeId[0]: %d\n", n4->outEdgeId ? n4->outEdgeId[0] : -999);
    printf("       DAC fanin_sum_node_id[0]: %d\n", 
           dac->fanin_sum_node_id ? dac->fanin_sum_node_id[0] : -999);
    
    if (dac->fanin_sum_node_id && dac->fanin_sum_node_id[0] >= 0) {
        int sum_id = dac->fanin_sum_node_id[0];
        printf("       SUM node %d validation: %s\n", sum_id, 
               (sum_id >= 0 && sum_id < lg->node_count) ? "VALID" : "INVALID");
        if (sum_id >= 0 && sum_id < lg->node_count) {
            RTNode *sum_node = &lg->nodes[sum_id];
            printf("       SUM node %d inputs: %d\n", sum_id, sum_node->nInputs);
            if (sum_node->inEdgeId) {
                for (int i = 0; i < sum_node->nInputs; i++) {
                    printf("       SUM Input[%d]: Edge %d\n", i, sum_node->inEdgeId[i]);
                }
            }
        }
    }
    
    bool step2_ok = apply_disconnect(lg, g_node4_id, 0, lg->dac_node_id, 0);
    printf("     N4→DAC disconnect result: %s\n", step2_ok ? "SUCCESS" : "FAILED");
    
    if (step2_ok) apply_graph_edits(lg->graphEditQueue, lg);
    dump_dac_edge_state("After N4→DAC disconnect attempt");
    
    // Continue with remaining steps
    bool step3_ok = graph_disconnect(lg, g_node2_id, 0, g_node3_id, 1);
    if (step3_ok) apply_graph_edits(lg->graphEditQueue, lg);
    
    bool step4_ok = apply_disconnect(lg, g_node3_id, 0, lg->dac_node_id, 0);
    if (step4_ok) apply_graph_edits(lg->graphEditQueue, lg);
    
    bool corruption_detected = !step1_ok || !step2_ok || !step3_ok || !step4_ok;
    if (corruption_detected) {
        printf("🚨 CORRUPTION: N1→N4=%s, N4→DAC=%s, N2→N3=%s, N3→DAC=%s\n",
               step1_ok ? "OK" : "FAIL", step2_ok ? "OK" : "FAIL",
               step3_ok ? "OK" : "FAIL", step4_ok ? "OK" : "FAIL");
    }
    
    return corruption_detected;
}

int main() {
    printf("🔬 Debug Analysis: Iteration 123 vs 124\n");
    printf("==========================================\n");
    
    initialize_reusable_graph();
    
    // Run silent iterations to build up corruption
    printf("📈 Building corruption state...\n");
    for (int i = 0; i < 121; i++) {  // Build up to iteration 121 like the stress test
        disconnect_all_edges();
        reconnect_full_topology();
        // Run disconnect sequence but don't debug
        bool step1_ok = graph_disconnect(g_reused_graph, g_node1_id, 0, g_node4_id, 0);
        if (step1_ok) apply_graph_edits(g_reused_graph->graphEditQueue, g_reused_graph);
        bool step2_ok = apply_disconnect(g_reused_graph, g_node4_id, 0, g_reused_graph->dac_node_id, 0);
        if (step2_ok) apply_graph_edits(g_reused_graph->graphEditQueue, g_reused_graph);
        bool step3_ok = graph_disconnect(g_reused_graph, g_node2_id, 0, g_node3_id, 1);
        if (step3_ok) apply_graph_edits(g_reused_graph->graphEditQueue, g_reused_graph);
        bool step4_ok = apply_disconnect(g_reused_graph, g_node3_id, 0, g_reused_graph->dac_node_id, 0);
        if (step4_ok) apply_graph_edits(g_reused_graph->graphEditQueue, g_reused_graph);
        
        if ((i + 1) % 20 == 0) printf("     Completed %d iterations...\n", i + 1);
    }
    
    printf("\n🔍 Now analyzing the critical transition:\n");
    
    // Now run iterations 122, 123, 124 with debug to catch the transition
    printf("\n" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "\n");
    bool corruption_122 = run_single_iteration_debug(122);
    
    printf("\n" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "\n");
    bool corruption_123 = run_single_iteration_debug(123);
    
    printf("\n" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "\n");
    bool corruption_124 = run_single_iteration_debug(124);
    
    printf("\n🎯 ANALYSIS RESULTS:\n");
    printf("====================\n");
    printf("Iteration 122: %s\n", corruption_122 ? "CORRUPTED" : "CLEAN");
    printf("Iteration 123: %s\n", corruption_123 ? "CORRUPTED" : "CLEAN");
    printf("Iteration 124: %s\n", corruption_124 ? "CORRUPTED" : "CLEAN");
    
    if (!corruption_123 && corruption_124) {
        printf("\n✅ PERFECT! Found the exact transition point where corruption begins!\n");
        printf("   This is the ideal place for lldb debugging to catch the corruption in action.\n");
    }
    
    destroy_live_graph(g_reused_graph);
    return corruption_124 ? 1 : 0;
}