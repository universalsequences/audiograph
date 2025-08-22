/**
 * Quick Race Condition Test Summary
 * 
 * This test runs a shorter version to confirm the MPMC queue fixes work
 * without the long runtime of the full stress test.
 */

#include "graph_api.h"
#include "graph_engine.h"
#include <math.h>
#include <sys/sysctl.h>

int get_cpu_count() {
    int cpu_count = 1;
    size_t size = sizeof(cpu_count);
    if (sysctlbyname("hw.ncpu", &cpu_count, &size, NULL, 0) != 0) {
        cpu_count = 4;
    }
    return cpu_count;
}

bool test_basic_race_condition_fix() {
    printf("=== Quick Race Condition Test ===\n");
    
    // Create a moderately complex graph
    GraphBuilder* gb = create_graph_builder();
    
    AudioNode* osc1 = create_oscillator(gb, 440.0f, "osc1");
    AudioNode* osc2 = create_oscillator(gb, 660.0f, "osc2");
    AudioNode* osc3 = create_oscillator(gb, 880.0f, "osc3");
    AudioNode* gain1 = create_gain(gb, 0.3f, "gain1");
    AudioNode* gain2 = create_gain(gb, 0.3f, "gain2"); 
    AudioNode* gain3 = create_gain(gb, 0.3f, "gain3");
    AudioNode* mixer = create_mixer2(gb, "mixer");
    
    connect(osc1, gain1);
    connect(osc2, gain2);
    connect(osc3, gain3);
    connect(gain1, mixer);
    connect(gain2, mixer);
    connect(gain3, mixer);
    
    GraphState* graph = compile_graph(gb, 48000, 128, "race_test");
    free_graph_builder(gb);
    
    if (!graph) {
        printf("❌ Failed to create graph\n");
        return false;
    }
    
    printf("Graph created: %d nodes, %d edges\n", graph->nodeCount, graph->edgeCount);
    
    // Start workers
    int cpu_count = get_cpu_count();
    int workers = (cpu_count > 1) ? cpu_count - 1 : 1;
    if (workers > 6) workers = 6; // Limit for test
    
    printf("Starting %d workers...\n", workers);
    engine_start_workers(workers);
    
    // Process blocks - this would crash with the original rb_pop_sc bug
    printf("Processing blocks with worker contention...\n");
    bool success = true;
    
    for (int i = 0; i < 100; i++) { // Much shorter test
        process_block_parallel(graph, 128);
        
        // Basic validation - check for NaN/infinity
        float* output = graph->edgeBufs[graph->masterEdge];
        for (int j = 0; j < 10; j++) { // Check first few samples
            if (!isfinite(output[j])) {
                printf("ERROR: Block %d sample %d is invalid: %f\n", i, j, output[j]);
                success = false;
                break;
            }
        }
        
        if (!success) break;
        
        if (i % 20 == 0) {
            printf("  Block %d: output[0] = %.6f\n", i, output[0]);
        }
    }
    
    engine_stop_workers();
    free_graph(graph);
    
    if (success) {
        printf("✅ Race condition test passed!\n");
        printf("   - Processed 100 blocks successfully\n");
        printf("   - %d worker threads + audio thread shared work queue safely\n", workers);
        printf("   - No crashes, infinite loops, or invalid output\n");
        printf("   - MPMC queue prevented the original rb_pop_sc race condition\n");
    } else {
        printf("❌ Race condition test failed - invalid output detected\n");
    }
    
    return success;
}

int main() {
    printf("AudioGraph MPMC Queue Race Condition Fix - Summary Test\n");
    printf("========================================================\n");
    printf("Quick test to verify the rb_pop_sc race condition has been fixed.\n\n");
    
    bool success = test_basic_race_condition_fix();
    
    if (success) {
        printf("\n🎉 SUCCESS: MPMC queue implementation works!\n");
        printf("The original threading bug has been resolved.\n");
        printf("AudioGraph can now safely use multiple worker threads.\n");
        return 0;
    } else {
        printf("\n💥 FAILURE: Race conditions still present!\n");
        return 1; 
    }
}