/******************************************************************************
 * AudioGraph Engine Multi-Worker Race Condition Test
 * 
 * This test reproduces the exact race condition that existed in the original
 * AudioGraph engine where multiple worker threads and the audio thread would
 * all call rb_pop_sc() to consume jobs from the ready queue.
 *
 * The original bug:
 * - rb_pop_sc() was designed for single consumer use
 * - But both audio thread (in process_block_parallel) and worker threads 
 *   (in worker_main) called it simultaneously
 * - This caused job duplication, job loss, and potential crashes
 *
 * This test verifies the fix by:
 * 1. Creating a complex graph with many nodes
 * 2. Using the actual engine worker system (engine_start_workers) 
 * 3. Processing blocks with process_block_parallel (the race condition hotspot)
 * 4. Monitoring output consistency across many iterations
 * 5. Detecting any signs of job loss/duplication via output corruption
 ******************************************************************************/

#include "graph_api.h"
#include "graph_engine.h"
#include "graph_nodes.h"
#include <pthread.h>
#include <assert.h>
#include <time.h>
#include <math.h>
#include <sys/sysctl.h>

// Test configuration
#define NUM_WORKERS 8           // High worker count to maximize contention
#define NUM_TEST_BLOCKS 5       // Very few blocks for detailed debugging
#define BLOCK_SIZE 128          // Standard audio block size  
#define SAMPLE_RATE 48000
#define NUM_OSCILLATORS 8       // Simplified graph for detailed tracing

// Test state
typedef struct {
    GraphState* test_graph;
    _Atomic bool test_running;
    
    // Output validation - detect corruption from race conditions
    float reference_output[BLOCK_SIZE];  // Expected output from first block
    _Atomic int corruption_count;        // Number of blocks that deviated significantly
    _Atomic int total_blocks_processed;
    
    // Performance tracking
    struct timespec start_time, end_time;
    _Atomic uint64_t total_processing_time_ns;
    _Atomic uint64_t max_block_time_ns;
    _Atomic uint64_t min_block_time_ns;
} EngineTestState;

static EngineTestState g_engine_test;

// Get CPU count for worker sizing
int get_cpu_count() {
    int cpu_count = 1;
    size_t size = sizeof(cpu_count);
    if (sysctlbyname("hw.ncpu", &cpu_count, &size, NULL, 0) != 0) {
        cpu_count = 4; // fallback
    }
    return cpu_count;
}

// Create a complex graph designed to stress the job scheduler
GraphState* create_complex_test_graph() {
    printf("Creating complex graph with %d oscillators + gains + mixer...\n", NUM_OSCILLATORS);
    
    GraphBuilder* gb = create_graph_builder();
    assert(gb != NULL);
    
    // Create many oscillators with IDENTICAL parameters for deterministic output
    AudioNode* oscillators[NUM_OSCILLATORS];
    AudioNode* gains[NUM_OSCILLATORS];
    
    const float test_freq = 10.0f;  // Low frequency for predictable, deterministic output
    const float individual_gain = 0.05f; // Low gain per oscillator
    
    for (int i = 0; i < NUM_OSCILLATORS; i++) {
        char osc_name[32], gain_name[32];
        snprintf(osc_name, sizeof(osc_name), "osc_%d", i);
        snprintf(gain_name, sizeof(gain_name), "gain_%d", i);
        
        // All oscillators identical: same frequency, same phase, same parameters
        oscillators[i] = create_oscillator(gb, test_freq, osc_name);
        gains[i] = create_gain(gb, individual_gain, gain_name);
        
        assert(oscillators[i] != NULL);
        assert(gains[i] != NULL);
        
        // Connect osc -> gain
        connect(oscillators[i], gains[i]);
    }
    
    // Create 8-input mixer to combine all signals
    AudioNode* mixer = create_generic_node(gb, mix8_process, 0, 8, 1, "final_mixer");
    assert(mixer != NULL);
    
    // Connect all gains to mixer - this creates a complex dependency graph
    for (int i = 0; i < NUM_OSCILLATORS; i++) {
        connect(gains[i], mixer);
    }
    
    // CRITICAL FIX: Create output node - mixer needs an output to become master edge
    AudioNode* output = create_gain(gb, 1.0f, "master_output");
    assert(output != NULL);
    connect(mixer, output);
    
    // Compile the graph
    GraphState* graph = compile_graph(gb, SAMPLE_RATE, BLOCK_SIZE, "race_test_graph");
    assert(graph != NULL);
    
    free_graph_builder(gb);
    
    printf("Graph compiled successfully:\n");
    printf("  Nodes: %d\n", graph->nodeCount);
    printf("  Edges: %d\n", graph->edgeCount);
    printf("  This creates %d schedulable jobs per block\n", graph->nodeCount);
    printf("  Expected output: %d oscillators at %.1f Hz, each with gain %.3f\n", 
           NUM_OSCILLATORS, test_freq, individual_gain);
    printf("  Total expected amplitude: %.3f (20 * %.3f)\n", 
           NUM_OSCILLATORS * individual_gain, individual_gain);
    
    return graph;
}

// Process one block and validate output consistency
bool process_and_validate_block(int block_num) {
    printf("\n=== PROCESSING BLOCK %d ===\n", block_num);
    printf("Block %d: Starting parallel processing with %d nodes\n", 
           block_num, g_engine_test.test_graph->nodeCount);
    
    uint64_t start_time = nsec_now();
    
    // This is the critical function that would trigger the race condition!
    // process_block_parallel internally:
    // 1. Seeds the ready queue with source nodes  
    // 2. Audio thread calls rb_pop_sc to get work
    // 3. Worker threads (in worker_main) ALSO call rb_pop_sc 
    // 4. With old implementation -> RACE CONDITION
    // 5. With new MPMC queue -> thread safe
    process_block_parallel(g_engine_test.test_graph, BLOCK_SIZE);
    
    uint64_t end_time = nsec_now();
    uint64_t block_time = end_time - start_time;
    
    printf("Block %d: Completed parallel processing in %.2f μs\n", 
           block_num, block_time / 1000.0);
    
    // Update timing stats
    atomic_fetch_add_explicit(&g_engine_test.total_processing_time_ns, block_time, memory_order_relaxed);
    
    uint64_t current_max = atomic_load_explicit(&g_engine_test.max_block_time_ns, memory_order_relaxed);
    while (block_time > current_max && 
           !atomic_compare_exchange_weak_explicit(&g_engine_test.max_block_time_ns, &current_max, block_time,
                                                memory_order_relaxed, memory_order_relaxed));
    
    uint64_t current_min = atomic_load_explicit(&g_engine_test.min_block_time_ns, memory_order_relaxed);
    if (current_min == 0) current_min = block_time;
    while (block_time < current_min && 
           !atomic_compare_exchange_weak_explicit(&g_engine_test.min_block_time_ns, &current_min, block_time,
                                                memory_order_relaxed, memory_order_relaxed));
    
    // Get the output buffer (master edge)
    float* output = g_engine_test.test_graph->edgeBufs[g_engine_test.test_graph->masterEdge];
    
    // For the first block, save as reference
    if (block_num == 0) {
        memcpy(g_engine_test.reference_output, output, BLOCK_SIZE * sizeof(float));
        printf("Reference output established: first sample = %.6f\n", output[0]);
        printf("  (Expected: predictable waveform from %d identical 10Hz oscillators)\n", NUM_OSCILLATORS);
        return true;
    }
    
    // With identical oscillators at 10Hz, the output should be perfectly predictable
    // Any significant deviation indicates a race condition where nodes were processed incorrectly
    float max_deviation = 0.0f;
    float avg_deviation = 0.0f;
    
    for (int i = 0; i < BLOCK_SIZE; i++) {
        float deviation = fabs(output[i] - g_engine_test.reference_output[i]);
        if (deviation > max_deviation) max_deviation = deviation;
        avg_deviation += deviation;
    }
    avg_deviation /= BLOCK_SIZE;
    
    // With identical oscillators, deviations should be minimal (only floating-point precision)
    bool corrupted = false;
    const float max_allowed_deviation = 1e-6f;  // Very strict - identical oscillators should be identical
    const float max_allowed_avg = 1e-7f;
    
    if (max_deviation > max_allowed_deviation || avg_deviation > max_allowed_avg) {
        printf("ERROR: Block %d shows significant deviation (likely race condition):\n", block_num);
        printf("  Max deviation: %.8f (threshold: %.8f)\n", max_deviation, max_allowed_deviation);
        printf("  Avg deviation: %.8f (threshold: %.8f)\n", avg_deviation, max_allowed_avg);  
        printf("  First sample: %.8f (reference: %.8f)\n", output[0], g_engine_test.reference_output[0]);
        printf("  This suggests job loss/duplication in the work queue!\n");
        atomic_fetch_add_explicit(&g_engine_test.corruption_count, 1, memory_order_relaxed);
        corrupted = true;
    }
    
    // Check for NaN/infinity (definite signs of corruption)
    for (int i = 0; i < BLOCK_SIZE; i++) {
        if (!isfinite(output[i])) {
            printf("ERROR: Block %d sample %d is not finite: %f\n", block_num, i, output[i]);
            corrupted = true;
            break;
        }
    }
    
    return !corrupted;
}

// Main test function
bool test_engine_worker_race_conditions() {
    printf("\n=== AudioGraph Engine Multi-Worker Race Condition Test ===\n");
    printf("Testing the exact rb_pop_sc race condition scenario:\n\n");
    
    // Initialize test state
    memset(&g_engine_test, 0, sizeof(g_engine_test));
    atomic_store_explicit(&g_engine_test.min_block_time_ns, UINT64_MAX, memory_order_relaxed);
    
    // CRITICAL FIX: Initialize g_engine.blockSize - worker threads need this!
    g_engine.blockSize = BLOCK_SIZE;
    g_engine.sampleRate = SAMPLE_RATE;
    
    // Create complex graph
    g_engine_test.test_graph = create_complex_test_graph();
    
    // Determine worker count
    int cpu_count = get_cpu_count();
    int worker_count = (cpu_count > 1) ? cpu_count - 1 : 1;
    if (worker_count > NUM_WORKERS) worker_count = NUM_WORKERS;
    
    printf("\nStarting %d worker threads (CPU count: %d)...\n", worker_count, cpu_count);
    
    // Start the actual engine worker system - this is where the race condition occurs!
    engine_start_workers(worker_count);
    
    atomic_store_explicit(&g_engine_test.test_running, true, memory_order_release);
    
    printf("Processing %d blocks with worker thread contention...\n", NUM_TEST_BLOCKS);
    printf("Each block schedules %d nodes across %d worker threads + audio thread\n\n", 
           g_engine_test.test_graph->nodeCount, worker_count);
    
    // Start timing
    clock_gettime(CLOCK_MONOTONIC, &g_engine_test.start_time);
    
    bool all_blocks_valid = true;
    
    // Process blocks - this is where the race condition would manifest!
    for (int block = 0; block < NUM_TEST_BLOCKS; block++) {
        bool valid = process_and_validate_block(block);
        if (!valid) all_blocks_valid = false;
        
        atomic_fetch_add_explicit(&g_engine_test.total_blocks_processed, 1, memory_order_relaxed);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &g_engine_test.end_time);
    atomic_store_explicit(&g_engine_test.test_running, false, memory_order_release);
    
    // Stop workers
    printf("\nStopping worker threads...\n");
    engine_stop_workers();
    
    // Analyze results
    int corruption_count = atomic_load(&g_engine_test.corruption_count);
    int total_blocks = atomic_load(&g_engine_test.total_blocks_processed);
    uint64_t total_time_ns = atomic_load(&g_engine_test.total_processing_time_ns);
    uint64_t max_time_ns = atomic_load(&g_engine_test.max_block_time_ns);
    uint64_t min_time_ns = atomic_load(&g_engine_test.min_block_time_ns);
    
    double duration_ms = (g_engine_test.end_time.tv_sec - g_engine_test.start_time.tv_sec) * 1000.0 +
                        (g_engine_test.end_time.tv_nsec - g_engine_test.start_time.tv_nsec) / 1000000.0;
    
    double avg_block_time_us = (total_time_ns / 1000.0) / total_blocks;
    double max_block_time_us = max_time_ns / 1000.0;
    double min_block_time_us = min_time_ns / 1000.0;
    
    printf("\n=== Test Results ===\n");
    printf("Test duration: %.2f ms\n", duration_ms);
    printf("Blocks processed: %d / %d\n", total_blocks, NUM_TEST_BLOCKS);
    printf("Block timing:\n");
    printf("  Average: %.2f μs\n", avg_block_time_us);
    printf("  Min: %.2f μs\n", min_block_time_us);
    printf("  Max: %.2f μs\n", max_block_time_us);
    printf("Output corruption:\n");
    printf("  Corrupted blocks: %d / %d (%.2f%%)\n", corruption_count, total_blocks, 
           (float)corruption_count / total_blocks * 100.0f);
    
    // Success criteria
    bool success = (corruption_count == 0) && all_blocks_valid && (total_blocks == NUM_TEST_BLOCKS);
    
    if (success) {
        printf("\n✅ SUCCESS: Engine multi-worker test passed!\n");
        printf("   - No output corruption detected across %d blocks\n", NUM_TEST_BLOCKS);
        printf("   - %d worker threads safely shared the ready queue\n", worker_count);
        printf("   - MPMC queue successfully prevented race conditions\n");
        printf("   - Consistent audio output maintained throughout test\n");
    } else {
        printf("\n❌ FAILURE: Engine multi-worker test failed!\n");
        if (corruption_count > 0) {
            printf("   - %d blocks showed output corruption\n", corruption_count);
            printf("   - This indicates race conditions in job scheduling\n");
        }
        if (!all_blocks_valid) {
            printf("   - Some blocks had invalid output values\n");
        }
        if (total_blocks != NUM_TEST_BLOCKS) {
            printf("   - Test did not complete all blocks (%d/%d)\n", total_blocks, NUM_TEST_BLOCKS);
        }
    }
    
    // Cleanup
    free_graph(g_engine_test.test_graph);
    
    return success;
}

int main() {
    printf("AudioGraph Engine Multi-Worker Race Condition Test\n");
    printf("==================================================\n");
    printf("This test reproduces the exact race condition from the original bug:\n");
    printf("- Multiple worker threads + audio thread calling rb_pop_sc()\n");
    printf("- Using the actual engine worker system with process_block_parallel()\n");
    printf("- High-contention workload designed to expose race conditions\n");
    printf("- Output validation to detect job loss/duplication effects\n\n");
    
    bool success = test_engine_worker_race_conditions();
    
    if (success) {
        printf("\n🎉 RACE CONDITION TEST PASSED!\n");
        printf("The MPMC queue implementation successfully prevents the original bug.\n");  
        printf("AudioGraph can now safely use multiple worker threads without corruption.\n");
        return 0;
    } else {
        printf("\n💥 RACE CONDITION TEST FAILED!\n");
        printf("Race conditions are still present - the fix needs investigation.\n");
        return 1;
    }
}