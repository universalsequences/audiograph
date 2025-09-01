# AudioGraph

A real-time audio graph processing engine written in C11 with lock-free multi-threaded architecture for live editing and parallel processing.

## Architecture Overview

AudioGraph is built around a **port-based connection model** with **multi-threaded worker pools** and **lock-free queue systems** for safe real-time audio processing and live graph editing.

**Important**: AudioGraph enforces a **Directed Acyclic Graph (DAG)** topology - feedback loops and cycles are not supported. Nodes that form cycles or are not reachable from the output (DAC) are automatically marked as orphaned and excluded from processing.

### Core Components

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   Main API      │    │  Queue Systems  │    │  Worker Pool    │
│                 │    │                 │    │                 │
│ • add_node()    │    │ • MPMC Queue    │    │ • Multi-thread  │
│ • connect()     │    │ • Param Ring    │    │ • Lock-free     │
│ • disconnect()  │    │ • Edit Queue    │    │ • Parallel DSP  │
│ • delete_node() │    │ • Port-based    │    │ • Work Stealing │
└─────────────────┘    └─────────────────┘    └─────────────────┘
         │                       │                       │
         └───────────────────────┼───────────────────────┘
                                 │
                    ┌─────────────────┐
                    │   Node System   │
                    │                 │
                    │ • NodeVTable    │
                    │ • RTNode        │
                    │ • State Memory  │
                    └─────────────────┘
```

### Multi-threaded Architecture

**Worker Pool**: Configurable number of worker threads process nodes in parallel
- **MPMC Queue**: Multi-Producer, Multi-Consumer work distribution
- **Lock-free Scheduling**: Atomic counters track node dependencies
- **Real-time Safe**: No allocations or locks in audio processing path

**Queue Systems**:
- **Edit Queue**: Batched graph modifications (`add_node`, `connect`, `disconnect`, `delete_node`)
- **Parameter Queue**: Real-time parameter updates (SPSC ring buffer)
- **Command Queue**: Thread-safe coordination between UI and audio threads

## Main Interface

The primary interface uses four core functions for graph manipulation:

### Node Management
```c
// Add a new node to the graph (returns pre-allocated node ID)
int add_node(LiveGraph *lg, NodeVTable vtable, void *state, 
             const char *name, int nInputs, int nOutputs);

// Remove a node and clean up all its connections
bool delete_node(LiveGraph *lg, int node_id);
```

### Port-based Connections
```c
// Connect specific ports between nodes
bool connect(LiveGraph *lg, int src_node, int src_port, 
             int dst_node, int dst_port);

// Disconnect specific port connections
bool disconnect(LiveGraph *lg, int src_node, int src_port, 
                int dst_node, int dst_port);
```

### Key Design Features

**Port-based Connections**: Each node has numbered input/output ports
- Precise connection control (not just node-to-node)
- Multiple connections per node supported
- Explicit port targeting prevents connection ambiguity

**Pre-allocated Node IDs**: Node creation returns immediately usable IDs
- Thread-safe atomic ID allocation
- No waiting for audio thread processing
- Failed nodes tracked separately for error handling

## Node System

### NodeVTable Interface

Every audio processing node implements a simple kernel interface:

```c
typedef struct {
    KernelFn  process;    // Core audio processing function
    InitFn    init;       // Optional: initialize state  
    ResetFn   reset;      // Optional: reset to initial state
    MigrateFn migrate;    // Optional: copy state during hot-swap
} NodeVTable;
```

### Node State Management

Node state is represented as **indexed float arrays** for efficient parameter updates:

```c
// Example: Oscillator state layout
#define OSC_MEMORY_SIZE 2
#define OSC_PHASE 0
#define OSC_INC   1

void osc_process(float* const* in, float* const* out, int n, void* memory) {
    float* mem = (float*)memory;
    float* y = out[0];
    
    for(int i = 0; i < n; i++) {
        y[i] = 2.0f * mem[OSC_PHASE] - 1.0f;  // Generate sawtooth
        mem[OSC_PHASE] += mem[OSC_INC];
        if(mem[OSC_PHASE] >= 1.f) mem[OSC_PHASE] -= 1.f;
    }
}
```

## Queue Architecture

### MPMC Work Queue

**Multi-Producer, Multi-Consumer** queue enables parallel node processing:
- **Vyukov-style** bounded queue with per-cell sequence numbers
- **ABA Protection**: Prevents race conditions in multi-threaded access
- **Cache-aligned**: 64-byte aligned cells for optimal memory performance

### Parameter Ring Buffer

**Single-Producer, Single-Consumer** ring for real-time parameter updates:
```c
typedef struct {
    uint64_t idx;        // Parameter index (direct memory access)
    uint64_t logical_id; // Target node identifier  
    float fvalue;        // New parameter value
} ParamMsg;
```

### Graph Edit Queue

**Thread-safe command queuing** for live graph modifications:
- Batched operations applied between audio blocks
- Commands: `GE_ADD_NODE`, `GE_REMOVE_NODE`, `GE_CONNECT`, `GE_DISCONNECT`
- Atomic application ensures graph consistency

## Usage Example

### Complete System Example

```c
#include "graph_engine.h"
#include "graph_edit.h"
#include "graph_nodes.h"
#include <stdio.h>

int main() {
    // 1. Initialize the engine
    initialize_engine(128, 48000);  // 128-sample blocks, 48kHz
    
    // 2. Create live graph with initial capacity (grows automatically)
    LiveGraph *lg = create_live_graph(16, 128, "my_audio_graph");
    
    // 3. Start worker threads for parallel processing
    engine_start_workers(4);  // Use 4 worker threads
    
    // 4. Build the audio graph (all operations are queued)
    int osc1 = live_add_oscillator(lg, 440.0f, "A4");       // 440Hz sine
    int osc2 = live_add_oscillator(lg, 660.0f, "E5");       // 660Hz sine  
    int gain1 = live_add_gain(lg, 0.3f, "vol1");            // 30% volume
    int gain2 = live_add_gain(lg, 0.2f, "vol2");            // 20% volume
    int mixer = live_add_mixer2(lg, "main_mix");             // 2-input mixer
    
    // 5. Connect the signal path (queued operations)
    connect(lg, osc1, 0, gain1, 0);           // osc1 -> gain1
    connect(lg, osc2, 0, gain2, 0);           // osc2 -> gain2  
    connect(lg, gain1, 0, mixer, 0);          // gain1 -> mixer input 0
    connect(lg, gain2, 0, mixer, 1);          // gain2 -> mixer input 1
    connect(lg, mixer, 0, lg->dac_node_id, 0); // mixer -> DAC output
    
    // 6. Process audio blocks and read output
    float output_buffer[128];
    for (int block = 0; block < 10; block++) {
        // Process one audio block (applies queued edits + runs DSP)
        process_next_block(lg, output_buffer, 128);
        
        // Print some output samples
        printf("Block %d: [%.6f, %.6f, %.6f, ...]\n", 
               block, output_buffer[0], output_buffer[1], output_buffer[2]);
    }
    
    // 7. Clean shutdown
    engine_stop_workers();        // Stop all worker threads
    destroy_live_graph(lg);       // Free all graph memory
    
    return 0;
}
```

### Live Editing While Audio Runs

```c
// Process audio in real-time (call from audio callback)
float output[128];
process_next_block(lg, output, 128);

// Edit graph while audio continues
disconnect(lg, gain1, 0, mixer, 0);  // Remove gain1 from mixer
connect(lg, osc1, 0, mixer, 0);      // Connect osc1 directly to mixer

// Add new processing chain
int filter = live_add_gain(lg, 0.8f, "filter");  // Could be any node type
connect(lg, osc2, 0, filter, 0);
disconnect(lg, gain2, 0, mixer, 1);
connect(lg, filter, 0, mixer, 1);
```

### Parameter Updates

```c
// Thread-safe parameter updates (non-blocking)
ParamMsg msg = {
    .idx = GAIN_VALUE,       // Direct memory index
    .logical_id = gain1,     // Target node ID
    .fvalue = 0.8f           // New gain value
};
params_push(lg->params, msg);

// Parameters applied automatically before each audio block
```

## Building

```bash
make                    # Build the project
make debug             # Debug build with symbols
make test              # Run all tests
make clean             # Clean build artifacts
```

### Test Programs

```bash
./test_queue_api                    # Test port-based API
./test_graph_edit_queue            # Test queued graph modifications
./test_disconnect                  # Test port-based disconnections
./test_deletion_safety             # Test node deletion with workers
./test_live_graph_partial_connections  # Test live editing under load
```

## Performance Characteristics

**Real-time Safe**: 
- Lock-free algorithms throughout
- No memory allocation in audio path
- Atomic operations for coordination

**Scalable**:
- Worker pool scales with CPU cores
- MPMC queue distributes work efficiently
- Port-based connections minimize graph traversal

**Low Latency**:
- Parameter updates apply within one audio block
- Graph edits batched and applied atomically
- Minimal overhead per node processing call

## Threading Model

- **Audio Thread**: Calls `process_next_block()` - the only real-time safe function
- **UI/Control Thread**: Graph editing (`add_node`, `connect`, etc.) and parameter updates
- **Worker Pool**: Parallel node processing when `engine_start_workers()` is active
- **Queue Processing**: Edit commands applied between audio blocks for consistency

⚠️ **Real-time Safety**: Only `process_next_block()` and `params_push()` are safe from audio threads.

## License

GPL v3 - See LICENSE file for details.