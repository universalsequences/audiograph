# AudioGraph

A small real-time audio graph engine written in C11 with lock-free multi-threaded architecture for live editing and parallel processing. 
- simple web-audio inspired API
- multi-threaded scheduling lets you take advantage of _all_ your computers cores
- add/delete nodes and edit connections safely while the engine is running
- auto summing inputs to nodes (inspired by web-audio and Max/MSP)
- simple API for adding "custom" nodes (using function pointers) to the graph
- easily embeddedable in swift projects (see swift integration guide)

## Architecture Overview

AudioGraph is built around a **port-based connection model** with **multi-threaded worker pools** and **lock-free queue systems** for safe real-time audio processing and live graph editing.

**Important**: AudioGraph enforces a **Directed Acyclic Graph (DAG)** topology - feedback loops and cycles are not supported. Nodes that form cycles or are not reachable from the output (DAC) are automatically marked as orphaned and excluded from processing.

**Dynamic Capacity**: The graph automatically grows its internal node arrays when needed. During capacity growth, all node port arrays (`inEdgeId`, `outEdgeId`, `succ`) are individually reallocated to prevent memory corruption - this ensures proper cleanup during teardown since each node owns its port arrays independently.

### Memory Layout & Regrowth Strategy

**Node Storage**: The `LiveGraph` uses parallel arrays for node data:
```c
RTNode    *nodes;        // Node structures with port pointers
atomic_int *pending;     // Scheduling counters  
int       *indegree;     // Connection counts
bool      *is_orphaned;  // Reachability flags
```

**Critical Boundary Check**: Growth triggers when `node_id >= node_capacity` (not `node_id > node_capacity`) because array indexes are 0-based. With capacity N, valid indexes are 0 to N-1, so index N requires immediate growth.

**Regrowth Process**:
1. **Allocate new arrays** at 2× capacity using `malloc` (not `realloc` to avoid partial corruption)
2. **Deep copy nodes**: Each node's port arrays (`inEdgeId`, `outEdgeId`, `succ`) are individually `malloc`'d and `memcpy`'d to prevent shared pointer corruption
3. **Preserve edge pool**: The shared edge buffer pool (`lg->edges`) remains unchanged - only node capacity grows
4. **Atomic pointer swap**: Update all array pointers and capacity in one operation
5. **Clean old memory**: Free old port arrays first, then old node arrays

This strategy prevents double-free errors during teardown since each node owns independent copies of its port arrays after growth.

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

### Auto-Summing (Multi-input Mixing)

**Transparent Signal Combining**: When multiple sources connect to the same input port, AudioGraph automatically creates hidden SUM nodes to combine the signals:

```c
// Example: Three oscillators feeding one gain node
int osc1 = live_add_oscillator(lg, 440.0f, "A4");
int osc2 = live_add_oscillator(lg, 554.0f, "C#5"); 
int osc3 = live_add_oscillator(lg, 659.0f, "E5");
int gain = live_add_gain(lg, 0.5f, "chord_vol");

// These connections automatically create a hidden SUM node
connect(lg, osc1, 0, gain, 0);  // Direct connection: osc1 -> gain
connect(lg, osc2, 0, gain, 0);  // Auto-SUM: creates SUM(osc1, osc2) -> gain  
connect(lg, osc3, 0, gain, 0);  // Auto-SUM: grows to SUM(osc1, osc2, osc3) -> gain
```

**Key Features**:
- **Transparent**: Users see logical connections, hidden SUM nodes are managed automatically
- **Dynamic Growth**: SUM nodes expand/shrink as connections are added/removed
- **Auto-Collapse**: When reduced to 1 input, SUM collapses back to direct connection
- **Proper Scheduling**: SUM nodes process before their destination in topological order
- **Memory Efficient**: SUM nodes are stateless and reuse existing edge buffers

**Implementation Note**: Auto-summing is implemented via the `fanin_sum_node_id` tracking array in `RTNode`. When `apply_connect()` detects multiple sources to the same destination port, it:
1. Creates a SUM node with appropriate input count
2. Redirects existing and new sources to the SUM inputs  
3. Connects SUM output to the original destination
4. Updates successor lists and indegree tracking for correct scheduling

This ensures that audio mixing happens automatically without requiring users to manually create and manage mixer nodes for common use cases.

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

## Custom Node Implementation

AudioGraph supports creating **custom audio processing nodes** with user-defined behavior. This is ideal for building extensible systems like visual patch editors (Max MSP-style) or gen-expr environments where users can define custom DSP algorithms.

### Creating a Custom Node

Custom nodes require three components:
1. **Process Function**: Core DSP algorithm
2. **State Memory**: Float array for parameters and internal state
3. **NodeVTable**: Function pointer table with optional lifecycle methods

### Complete Custom Node Example

Here's a **delay line node** with adjustable delay time and feedback:

```c
// === Delay Node State Layout ===
#define DELAY_MEMORY_SIZE 4098  // 4096 samples + 2 params
#define DELAY_TIME_SAMPLES 0    // Parameter: delay time in samples
#define DELAY_FEEDBACK 1        // Parameter: feedback amount (0.0-0.99)
#define DELAY_WRITE_POS 2       // Internal: write position in buffer
#define DELAY_BUFFER_START 3    // Start of delay buffer (4096 samples)
#define DELAY_BUFFER_SIZE 4096

// === Custom Process Function ===
void delay_process(float* const* in, float* const* out, int n, void* memory) {
    float* mem = (float*)memory;
    float* input = in[0];
    float* output = out[0];
    
    float delay_time = mem[DELAY_TIME_SAMPLES];
    float feedback = mem[DELAY_FEEDBACK];
    int write_pos = (int)mem[DELAY_WRITE_POS];
    float* buffer = &mem[DELAY_BUFFER_START];
    
    for(int i = 0; i < n; i++) {
        // Calculate read position
        int read_pos = write_pos - (int)delay_time;
        if(read_pos < 0) read_pos += DELAY_BUFFER_SIZE;
        
        // Read delayed sample
        float delayed = buffer[read_pos];
        
        // Write input + feedback to buffer
        buffer[write_pos] = input[i] + (delayed * feedback);
        
        // Output = input + delayed signal
        output[i] = input[i] + delayed * 0.5f;
        
        // Advance write position
        write_pos = (write_pos + 1) % DELAY_BUFFER_SIZE;
    }
    
    // Update write position
    mem[DELAY_WRITE_POS] = (float)write_pos;
}

// === Optional: Initialize State ===
void delay_init(void* memory, int sample_rate, int max_block) {
    float* mem = (float*)memory;
    
    // Set default parameters
    mem[DELAY_TIME_SAMPLES] = sample_rate * 0.25f;  // 250ms delay
    mem[DELAY_FEEDBACK] = 0.3f;                     // 30% feedback
    mem[DELAY_WRITE_POS] = 0.0f;                    // Start at buffer beginning
    
    // Clear delay buffer
    memset(&mem[DELAY_BUFFER_START], 0, DELAY_BUFFER_SIZE * sizeof(float));
}

// === Optional: Reset to Initial State ===
void delay_reset(void* memory) {
    delay_init(memory, 48000, 512);  // Reset with default values
}

// === Create VTable ===
const NodeVTable DELAY_VTABLE = {
    .process = delay_process,
    .init = delay_init,      // Optional: called once after node creation
    .reset = delay_reset,    // Optional: called when graph is reset
    .migrate = NULL          // Optional: copy state during hot-swap
};

// === Integration with AudioGraph ===
int create_custom_delay_node(LiveGraph* lg, float delay_seconds, float feedback_amount, const char* name) {
    // Allocate and initialize state memory
    float* state = (float*)calloc(DELAY_MEMORY_SIZE, sizeof(float));
    if (!state) return -1;
    
    // Set initial parameters
    state[DELAY_TIME_SAMPLES] = delay_seconds * 48000.0f;  // Convert to samples
    state[DELAY_FEEDBACK] = feedback_amount;
    state[DELAY_WRITE_POS] = 0.0f;
    
    // Add node to live graph
    int node_id = add_node(lg, DELAY_VTABLE, state, name, 1, 1);  // 1 input, 1 output
    
    return node_id;
}
```

### Using Custom Nodes in Your Graph

```c
// Create live graph
LiveGraph* lg = create_live_graph(16, 128, "custom_graph");

// Add built-in nodes
int osc = live_add_oscillator(lg, 440.0f, "source");
int gain = live_add_gain(lg, 0.8f, "volume");

// Add your custom delay node
int delay = create_custom_delay_node(lg, 0.25f, 0.4f, "echo");

// Connect: oscillator -> delay -> gain -> output
connect(lg, osc, 0, delay, 0);     // osc -> delay input
connect(lg, delay, 0, gain, 0);    // delay -> gain
connect(lg, gain, 0, lg->dac_node_id, 0);  // gain -> DAC

// Process audio with your custom effect
float output[128];
process_next_block(lg, output, 128);
```

### Real-time Parameter Updates

Update custom node parameters safely during audio processing:

```c
// Update delay time parameter
ParamMsg delay_time_msg = {
    .idx = DELAY_TIME_SAMPLES,           // Parameter index in state array
    .logical_id = delay,                 // Target node ID
    .fvalue = 0.5f * 48000.0f           // New delay time (500ms in samples)
};
params_push(lg->params, delay_time_msg);

// Update feedback amount
ParamMsg feedback_msg = {
    .idx = DELAY_FEEDBACK,
    .logical_id = delay,
    .fvalue = 0.7f                      // 70% feedback
};
params_push(lg->params, feedback_msg);
```

### Advanced Custom Node Patterns

#### 1. Multi-Output Node (Stereo Processor)
```c
#define STEREO_MEMORY_SIZE 2
#define STEREO_WIDTH 0
#define STEREO_PHASE 1

void stereo_width_process(float* const* in, float* const* out, int n, void* memory) {
    float* mem = (float*)memory;
    float* mono_in = in[0];
    float* left_out = out[0];
    float* right_out = out[1];
    
    float width = mem[STEREO_WIDTH];
    float phase = mem[STEREO_PHASE];
    
    for(int i = 0; i < n; i++) {
        float mono = mono_in[i];
        left_out[i] = mono + (mono * width * sinf(phase));
        right_out[i] = mono - (mono * width * sinf(phase));
        phase += 0.001f;  // Slow LFO
    }
    
    mem[STEREO_PHASE] = phase;
}

// Usage: 1 input, 2 outputs
int stereo_node = add_node(lg, STEREO_WIDTH_VTABLE, state, "stereo", 1, 2);
```

#### 2. Multi-Input Node (Custom Mixer with Effects)
```c
void custom_mixer_process(float* const* in, float* const* out, int n, void* memory) {
    float* mem = (float*)memory;
    float* output = out[0];
    
    float gain1 = mem[0];
    float gain2 = mem[1];
    float reverb_send = mem[2];
    
    for(int i = 0; i < n; i++) {
        float mix = (in[0][i] * gain1) + (in[1][i] * gain2);
        
        // Simple reverb simulation
        float reverb = mix * reverb_send * 0.3f;
        output[i] = mix + reverb;
    }
}

// Usage: 2 inputs, 1 output
int mixer_node = add_node(lg, CUSTOM_MIXER_VTABLE, state, "fx_mixer", 2, 1);
```

### Integration with Visual Patch Editors

This custom node system is perfect for **Max MSP-style patch editors** where users:

1. **Define Custom Expressions**: Users write DSP code that gets compiled into process functions
2. **Dynamic Node Creation**: Each patch element becomes a custom node with its own vtable
3. **Live Parameter Binding**: Patch UI controls map directly to state memory indices
4. **Hot-Swapping**: Use `migrate` function to preserve state when recompiling expressions

```c
// Example: User-defined expression becomes a custom node
// Expression: "out = in * sin(phase) * gain; phase += freq"

void user_expr_process(float* const* in, float* const* out, int n, void* memory) {
    float* mem = (float*)memory;
    float* input = in[0];
    float* output = out[0];
    
    float gain = mem[0];    // Mapped to UI slider
    float freq = mem[1];    // Mapped to UI knob
    float phase = mem[2];   // Internal state
    
    for(int i = 0; i < n; i++) {
        output[i] = input[i] * sinf(phase) * gain;
        phase += freq;
    }
    
    mem[2] = phase;  // Save updated state
}

// This node can be created dynamically from user expressions
int expr_node = add_node(lg, USER_EXPR_VTABLE, expr_state, "user_expr", 1, 1);
```

### Best Practices for Custom Nodes

**State Memory Layout**:
- **Parameters first**: User-controllable values at known indices
- **Internal state last**: Algorithm-specific working variables
- **Alignment**: Use float arrays for SIMD-friendly memory access

**Thread Safety**:
- **Process function**: Must be real-time safe (no allocations, no locks)
- **Parameter updates**: Use `params_push()` for thread-safe parameter changes
- **State migration**: Copy persistent state when hot-swapping nodes

**Memory Management**:
- **Caller owns state**: Your code allocates and manages state memory
- **Cleanup**: Free state memory when done (graph doesn't auto-free custom state)
- **Initialization**: Always zero-initialize state or use `init` function

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

### Block Boundary Edit Processing

**Safe Graph Modification**: All graph edits are applied atomically at block boundaries to ensure real-time safety:

```c
void process_next_block(LiveGraph *lg, float *output_buffer, int nframes) {
    // 1. Apply all queued parameter updates first
    apply_params(lg->params);
    
    // 2. Apply all queued graph edits (add/remove/connect/disconnect)
    apply_graph_edits(lg->graphEditQueue, lg);
    
    // 3. Process audio with the updated graph structure
    process_live_block(lg, nframes);
    
    // 4. Copy final audio output to user buffer
    // ...
}
```

**Real-time Safety Guarantees**:
- **UI/Control Thread**: Pushes edit commands to lock-free queue (`add_node()`, `connect()`, etc.)
- **Audio Thread**: Drains entire queue between audio blocks via `apply_graph_edits()`
- **Zero Allocations**: All memory allocation (including capacity growth) happens between blocks, never during audio processing
- **Zero Locks**: Lock-free queue allows non-blocking command submission from any thread
- **Deterministic Timing**: Graph structure is frozen during each audio block - no mid-block topology changes
- **Consistent State**: Each audio block sees a stable, consistent graph configuration

**Edit Command Lifecycle**:
1. **Queue Phase**: Commands pushed to `GraphEditQueue` (returns immediately)
2. **Batch Apply**: All pending edits applied atomically at next block boundary  
3. **Capacity Growth**: Node arrays automatically expand if needed during add operations
4. **Dependency Update**: Orphan detection and scheduling arrays updated after structural changes

This design ensures that graph modifications never interfere with ongoing audio processing while maintaining deterministic real-time performance.

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
./tests/test_queue_api                    # Test port-based API
./tests/test_graph_edit_queue            # Test queued graph modifications
./tests/test_disconnect                  # Test port-based disconnections
./tests/test_deletion_safety             # Test node deletion with workers
./tests/test_live_graph_partial_connections  # Test live editing under load
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
