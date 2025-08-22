/******************************************************************************
 * AudioGraph Live Graph Multi-Worker Race Condition Test
 * 
 * This test is equivalent to test_engine_workers.c but uses the LiveGraph
 * system instead of compiled GraphState. It reproduces the exact race 
 * condition scenario that existed in the original engine but tests it
 * with the new multi-threaded LiveGraph processing.
 *
 * The original bug:
 * - rb_pop_sc() was designed for single consumer use
 * - But both audio thread and worker threads called it simultaneously  
 * - This caused job duplication, job loss, and potential crashes
 *
 * This test verifies the fix by:
 * 1. Creating a complex live graph with many nodes
 * 2. Using multi-threaded process_live_block (the race condition hotspot)
 * 3. Processing blocks with worker thread contention
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
#define NUM_TEST_BLOCKS 5       // Same as engine test for detailed debugging
#define BLOCK_SIZE 128          // Standard audio block size  
#define SAMPLE_RATE 48000
#define NUM_OSCILLATORS 8       // Create 8 oscillators to match engine test complexity

// Test state
typedef struct {
    LiveGraph* live_graph;
    _Atomic bool test_running;
    
    // Node IDs for the live graph
    int oscillator_nodes[NUM_OSCILLATORS];
    int gain_nodes[NUM_OSCILLATORS];
    int mixer_node;
    int output_node;
    
    // Output validation - detect corruption from race conditions
    float reference_output[BLOCK_SIZE];  // Expected output from first block
    _Atomic int corruption_count;        // Number of blocks that deviated significantly
    _Atomic int total_blocks_processed;
    
    // Performance tracking
    struct timespec start_time, end_time;
    _Atomic uint64_t total_processing_time_ns;
    _Atomic uint64_t max_block_time_ns;
    _Atomic uint64_t min_block_time_ns;
} LiveGraphTestState;

static LiveGraphTestState g_live_graph_test;

// Get CPU count for worker sizing
int get_cpu_count() {
    int cpu_count = 1;
    size_t size = sizeof(cpu_count);
    if (sysctlbyname("hw.ncpu", &cpu_count, &size, NULL, 0) != 0) {
        cpu_count = 4; // fallback
    }
    return cpu_count;
}

// Create a complex live graph designed to stress the job scheduler
bool create_complex_live_graph() {
    printf("Creating complex live graph with %d oscillators + gains + mixer...\\n", NUM_OSCILLATORS);
    
    g_live_graph_test.live_graph = create_live_graph(32, BLOCK_SIZE, "race_test_live_graph");
    if (!g_live_graph_test.live_graph) {
        printf("ERROR: Failed to create live graph\\n");
        return false;
    }
    
    // Create many oscillators with IDENTICAL parameters for deterministic output
    const float test_freq = 10.0f;  // Low frequency for predictable, deterministic output
    const float individual_gain = 0.05f; // Low gain per oscillator
    
    for (int i = 0; i < NUM_OSCILLATORS; i++) {
        char osc_name[32], gain_name[32];
        snprintf(osc_name, sizeof(osc_name), "live_osc_%d", i);
        snprintf(gain_name, sizeof(gain_name), "live_gain_%d", i);
        
        // All oscillators identical: same frequency, same phase, same parameters
        g_live_graph_test.oscillator_nodes[i] = live_add_oscillator(g_live_graph_test.live_graph, test_freq, osc_name);
        g_live_graph_test.gain_nodes[i] = live_add_gain(g_live_graph_test.live_graph, individual_gain, gain_name);
        
        if (g_live_graph_test.oscillator_nodes[i] < 0 || g_live_graph_test.gain_nodes[i] < 0) {
            printf("ERROR: Failed to create oscillator %d or gain %d\\n", i, i);
            return false;
        }
        
        // Connect osc -> gain
        if (!live_connect(g_live_graph_test.live_graph, g_live_graph_test.oscillator_nodes[i], g_live_graph_test.gain_nodes[i])) {
            printf("ERROR: Failed to connect oscillator %d to gain %d\\n", i, i);
            return false;
        }
    }
    
    // Create 8-input mixer (equivalent to the engine test)
    g_live_graph_test.mixer_node = live_add_mixer8(g_live_graph_test.live_graph, "live_mixer");
    if (g_live_graph_test.mixer_node < 0) {
        printf("ERROR: Failed to create 8-input mixer node\\n");
        return false;
    }
    
    // Connect all gains to mixer
    for (int i = 0; i < NUM_OSCILLATORS; i++) {
        if (!live_connect(g_live_graph_test.live_graph, g_live_graph_test.gain_nodes[i], g_live_graph_test.mixer_node)) {
            printf("ERROR: Failed to connect gain %d to mixer\\n", i);
            return false;
        }
    }
    
    // Create output node - mixer needs an output to become master edge
    g_live_graph_test.output_node = live_add_gain(g_live_graph_test.live_graph, 1.0f, "master_output");
    if (g_live_graph_test.output_node < 0) {
        printf("ERROR: Failed to create output node\\n");
        return false;
    }
    
    if (!live_connect(g_live_graph_test.live_graph, g_live_graph_test.mixer_node, g_live_graph_test.output_node)) {
        printf("ERROR: Failed to connect mixer to output\\n");
        return false;
    }
    
    printf("Live graph created successfully:\\n");
    printf("  Nodes: %d\\n", g_live_graph_test.live_graph->node_count);
    printf("  This creates %d schedulable jobs per block\\n", g_live_graph_test.live_graph->node_count);
    printf("  Expected output: %d oscillators at %.1f Hz, each with gain %.3f\\n", 
           NUM_OSCILLATORS, test_freq, individual_gain);
    printf("  Total expected amplitude: %.3f (%d * %.3f)\\n", 
           NUM_OSCILLATORS * individual_gain, NUM_OSCILLATORS, individual_gain);
    
    return true;
}

// Process one block and validate output consistency
bool process_and_validate_live_block(int block_num) {
    printf("\\n=== PROCESSING LIVE BLOCK %d ===\\n", block_num);
    printf("Live block %d: Starting parallel processing with %d nodes\\n", 
           block_num, g_live_graph_test.live_graph->node_count);
    
    uint64_t start_time = nsec_now();
    
    // This is the critical function that would trigger the race condition!
    // process_live_block internally:
    // 1. Seeds the ready queue with source nodes  
    // 2. Audio thread calls mpmc_pop to get work
    // 3. Worker threads (in worker_main) ALSO call mpmc_pop 
    // 4. With old implementation -> RACE CONDITION
    // 5. With new MPMC queue -> thread safe
    process_live_block(g_live_graph_test.live_graph, BLOCK_SIZE);
    
    uint64_t end_time = nsec_now();
    uint64_t block_time = end_time - start_time;
    
    printf("Live block %d: Completed parallel processing in %.2f μs\\n", 
           block_num, block_time / 1000.0);
    
    // Update timing stats
    atomic_fetch_add_explicit(&g_live_graph_test.total_processing_time_ns, block_time, memory_order_relaxed);
    
    uint64_t current_max = atomic_load_explicit(&g_live_graph_test.max_block_time_ns, memory_order_relaxed);
    while (block_time > current_max && 
           !atomic_compare_exchange_weak_explicit(&g_live_graph_test.max_block_time_ns, &current_max, block_time,
                                                memory_order_relaxed, memory_order_relaxed));
    
    uint64_t current_min = atomic_load_explicit(&g_live_graph_test.min_block_time_ns, memory_order_relaxed);
    if (current_min == 0) current_min = block_time;
    while (block_time < current_min && 
           !atomic_compare_exchange_weak_explicit(&g_live_graph_test.min_block_time_ns, &current_min, block_time,
                                                memory_order_relaxed, memory_order_relaxed));
    
    // Get the output buffer (master edge)
    int output_node = find_live_output(g_live_graph_test.live_graph);
    if (output_node < 0) {
        printf("ERROR: Could not find output node\\n");
        return false;
    }
    
    RTNode* node = &g_live_graph_test.live_graph->nodes[output_node];
    if (node->nInputs == 0) {
        printf("ERROR: Output node has no inputs\\n");
        return false;
    }
    
    int master_edge = node->inEdges[0];
    float* output = g_live_graph_test.live_graph->edge_buffers[master_edge];
    
    // For the first block, save as reference
    if (block_num == 0) {
        memcpy(g_live_graph_test.reference_output, output, BLOCK_SIZE * sizeof(float));
        printf("Reference output established: first sample = %.6f\\n", output[0]);
        printf("  (Expected: predictable waveform from %d identical 10Hz oscillators)\\n", NUM_OSCILLATORS);
        return true;
    }
    
    // Check if oscillators are synchronized - all should have advanced by the same amount
    // With 10Hz oscillators at 48kHz sample rate, each block advances phase by: 
    // 128 samples * (10Hz / 48000Hz) = 128 * 0.000208333 = 0.0266667 phase units
    // Expected output = NUM_OSCILLATORS * 0.05 * sawtooth(0.0266667 * block_num) 
    float expected_phase = 0.026666667f * block_num; // Phase advancement per block
    while (expected_phase >= 1.0f) expected_phase -= 1.0f; // Wrap phase
    float expected_sample = 2.0f * expected_phase - 1.0f; // Convert to sawtooth [-1, 1]
    float expected_output = NUM_OSCILLATORS * 0.05f * expected_sample; // All oscillators * gain
    
    // Allow for small floating-point errors in the expected calculation
    float deviation = fabs(output[0] - expected_output);
    bool corrupted = false;
    const float max_allowed_deviation = 0.001f; // Relaxed threshold for floating-point precision
    
    if (deviation > max_allowed_deviation) {
        printf("ERROR: Live block %d output doesn't match expected synchronized oscillators:\\n", block_num);
        printf("  Actual: %.8f\\n", output[0]); 
        printf("  Expected: %.8f (phase=%.6f)\\n", expected_output, expected_phase);
        printf("  Deviation: %.8f (threshold: %.8f)\\n", deviation, max_allowed_deviation);
        printf("  This suggests oscillators are not synchronized (race condition)!\\n");
        atomic_fetch_add_explicit(&g_live_graph_test.corruption_count, 1, memory_order_relaxed);
        corrupted = true;
    } else {
        printf("Live block %d: Output matches expected synchronized oscillators (deviation: %.8f)\\n", 
               block_num, deviation);
    }
    
    // Check for NaN/infinity (definite signs of corruption)
    for (int i = 0; i < BLOCK_SIZE; i++) {
        if (!isfinite(output[i])) {
            printf("ERROR: Live block %d sample %d is not finite: %f\\n", block_num, i, output[i]);
            corrupted = true;
            break;
        }
    }
    
    return !corrupted;
}

// Main live graph multi-worker test function
bool test_live_graph_worker_race_conditions() {
    printf("\\n=== AudioGraph Live Graph Multi-Worker Race Condition Test ===\\n");
    printf("Testing the exact rb_pop_sc race condition scenario with LiveGraph:\\n\\n");
    
    // Initialize test state
    memset(&g_live_graph_test, 0, sizeof(g_live_graph_test));
    atomic_store_explicit(&g_live_graph_test.min_block_time_ns, UINT64_MAX, memory_order_relaxed);
    
    // Initialize global engine
    g_engine.blockSize = BLOCK_SIZE;
    g_engine.sampleRate = SAMPLE_RATE;
    
    // Create complex live graph
    if (!create_complex_live_graph()) {
        printf("Failed to create test live graph\\n");
        return false;
    }
    
    // Determine worker count
    int cpu_count = get_cpu_count();
    int worker_count = (cpu_count > 1) ? cpu_count - 1 : 1;
    if (worker_count > NUM_WORKERS) worker_count = NUM_WORKERS;
    
    printf("\\nStarting %d worker threads (CPU count: %d)...\\n", worker_count, cpu_count);
    
    // Start the actual engine worker system - this is where the race condition occurs!
    engine_start_workers(worker_count);
    
    atomic_store_explicit(&g_live_graph_test.test_running, true, memory_order_release);
    
    printf("Processing %d live blocks with worker thread contention...\\n", NUM_TEST_BLOCKS);
    printf("Each block schedules %d nodes across %d worker threads + audio thread\\n\\n", 
           g_live_graph_test.live_graph->node_count, worker_count);
    
    // Start timing
    clock_gettime(CLOCK_MONOTONIC, &g_live_graph_test.start_time);
    
    bool all_blocks_valid = true;
    
    // Process blocks - this is where the race condition would manifest!
    for (int block = 0; block < NUM_TEST_BLOCKS; block++) {
        bool valid = process_and_validate_live_block(block);
        if (!valid) all_blocks_valid = false;
        
        atomic_fetch_add_explicit(&g_live_graph_test.total_blocks_processed, 1, memory_order_relaxed);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &g_live_graph_test.end_time);
    atomic_store_explicit(&g_live_graph_test.test_running, false, memory_order_release);
    
    // Stop workers
    printf("\\nStopping worker threads...\\n");
    engine_stop_workers();
    
    // Analyze results
    int corruption_count = atomic_load(&g_live_graph_test.corruption_count);
    int total_blocks = atomic_load(&g_live_graph_test.total_blocks_processed);
    uint64_t total_time_ns = atomic_load(&g_live_graph_test.total_processing_time_ns);
    uint64_t max_time_ns = atomic_load(&g_live_graph_test.max_block_time_ns);
    uint64_t min_time_ns = atomic_load(&g_live_graph_test.min_block_time_ns);
    
    double duration_ms = (g_live_graph_test.end_time.tv_sec - g_live_graph_test.start_time.tv_sec) * 1000.0 +
                        (g_live_graph_test.end_time.tv_nsec - g_live_graph_test.start_time.tv_nsec) / 1000000.0;
    
    double avg_block_time_us = (total_time_ns / 1000.0) / total_blocks;
    double max_block_time_us = max_time_ns / 1000.0;
    double min_block_time_us = min_time_ns / 1000.0;
    
    printf("\\n=== Live Graph Test Results ===\\n");
    printf("Test duration: %.2f ms\\n", duration_ms);
    printf("Blocks processed: %d / %d\\n", total_blocks, NUM_TEST_BLOCKS);
    printf("Block timing:\\n");
    printf("  Average: %.2f μs\\n", avg_block_time_us);
    printf("  Min: %.2f μs\\n", min_block_time_us);
    printf("  Max: %.2f μs\\n", max_block_time_us);
    printf("Output corruption:\\n");
    printf("  Corrupted blocks: %d / %d (%.2f%%)\\n", corruption_count, total_blocks, 
           (float)corruption_count / total_blocks * 100.0f);
    
    // Success criteria
    bool success = (corruption_count == 0) && all_blocks_valid && (total_blocks == NUM_TEST_BLOCKS);
    
    if (success) {
        printf("\\n✅ SUCCESS: Live graph multi-worker test passed!\\n");
        printf("   - No output corruption detected across %d blocks\\n", NUM_TEST_BLOCKS);
        printf("   - %d worker threads safely shared the ready queue\\n", worker_count);
        printf("   - MPMC queue successfully prevented race conditions\\n");
        printf("   - Consistent audio output maintained throughout test\\n");
        printf("   - LiveGraph equivalent to GraphState test: PASSED\\n");
    } else {
        printf("\\n❌ FAILURE: Live graph multi-worker test failed!\\n");
        if (corruption_count > 0) {
            printf("   - %d blocks showed output corruption\\n", corruption_count);
            printf("   - This indicates race conditions in job scheduling\\n");
        }
        if (!all_blocks_valid) {
            printf("   - Some blocks had invalid output values\\n");
        }
        if (total_blocks != NUM_TEST_BLOCKS) {
            printf("   - Test did not complete all blocks (%d/%d)\\n", total_blocks, NUM_TEST_BLOCKS);
        }
    }
    
    return success;
}

int main() {
    printf("AudioGraph Live Graph Multi-Worker Race Condition Test\\n");
    printf("======================================================\\n");
    printf("This test is equivalent to test_engine_workers.c but uses LiveGraph:\\n");
    printf("- Multiple worker threads + audio thread calling mpmc_pop()\\n");
    printf("- Using process_live_block() with worker thread system\\n");
    printf("- High-contention workload designed to expose race conditions\\n");
    printf("- Output validation to detect job loss/duplication effects\\n\\n");
    
    bool success = test_live_graph_worker_race_conditions();
    
    if (success) {
        printf("\\n🎉 LIVE GRAPH RACE CONDITION TEST PASSED!\\n");
        printf("The MPMC queue implementation successfully prevents race conditions in LiveGraph.\\n");  
        printf("LiveGraph can now safely use multiple worker threads without corruption.\\n");
        return 0;
    } else {
        printf("\\n💥 LIVE GRAPH RACE CONDITION TEST FAILED!\\n");
        printf("Race conditions are still present - the fix needs investigation.\\n");
        return 1;
    }
}