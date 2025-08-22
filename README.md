# AudioGraph

A modular, real-time audio graph processing engine written in C11 with Swift integration support.

## Motivation

AudioGraph was created to address fundamental limitations in existing audio frameworks:

### AVAudioEngine Limitations
- **Graph Restart Required**: Disconnecting certain node types forces a complete engine restart, causing audio dropouts
- **No Live Editing**: Cannot safely modify the graph structure while audio is playing
- **Single-threaded Processing**: All nodes process sequentially, wasting multi-core CPU potential
- **Limited Node Types**: Restricted to Apple's predefined node categories

### JUCE Limitations  
- **No Multi-threading**: Audio graph processing is fundamentally single-threaded
- **Complex Live Editing**: Modifying graphs during playback requires careful manual synchronization
- **Performance Bottlenecks**: Large graphs become CPU-bound on single cores

### AudioGraph Solution
- ✅ **True Live Editing**: Add, remove, and reconnect nodes while audio streams continuously
- ✅ **Multi-threaded Engine**: Parallel node processing with lock-free scheduling
- ✅ **Zero-dropout Hot-swapping**: Replace entire graphs with seamless crossfading  
- ✅ **Web Audio-style API**: Intuitive `connect()` and `disconnect()` operations
- ✅ **Real-time Safe**: No allocations or locks in audio callback path

## Features

- **Modular Architecture**: Clean separation between API, engine, and DSP nodes
- **Real-time Processing**: Lock-free MPSC scheduling with configurable worker thread pool
- **Live Editing**: Web Audio-style dynamic graph modification while audio is running
- **Hot-swapping**: Seamless graph replacement with crossfading (no dropouts)
- **Swift Ready**: C API designed for easy Swift/Core Audio integration
- **High Performance**: Parallel processing scales with CPU cores

## Architecture

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   graph_api.h   │    │  graph_engine.h │    │  graph_nodes.h  │
│                 │    │                 │    │                 │
│ • AudioNode     │    │ • GraphState    │    │ • Oscillator    │
│ • GraphBuilder  │    │ • LiveGraph     │    │ • Gain          │
│ • connect()     │    │ • Engine        │    │ • Mixer         │
└─────────────────┘    └─────────────────┘    └─────────────────┘
         │                       │                       │
         └───────────────────────┼───────────────────────┘
                                 │
                    ┌─────────────────┐
                    │  graph_types.h  │
                    │                 │
                    │ • NodeVTable    │
                    │ • ParamRing     │
                    │ • Helpers       │
                    └─────────────────┘
```

## Node Kernel Design

AudioGraph achieves its flexibility and performance through a simple **kernel abstraction**. Every audio processing node is represented by a lightweight kernel with a uniform interface:

### The NodeVTable Interface

```c
typedef struct {
    KernelFn  process;    // Core audio processing function
    InitFn    init;       // Optional: initialize state  
    ResetFn   reset;      // Optional: reset to initial state
    MigrateFn migrate;    // Optional: copy state during hot-swap
} NodeVTable;
```

### Kernel Function Signature

```c
void process(float* const* inputs,   // Array of input buffers
             float* const* outputs,  // Array of output buffers  
             int nframes,            // Number of samples to process
             void* state);           // Node's private state
```

### Benefits of the Kernel Model

**🔧 Simplicity**: Each node is just a function that processes audio buffers
- No inheritance hierarchies or complex object models
- Easy to understand, debug, and optimize
- Minimal API surface area

**⚡ Performance**: Direct function calls with zero virtual dispatch overhead
- Compiles to efficient machine code  
- Cache-friendly memory layout
- No allocations in processing path

**🔄 Hot-swappable**: Nodes can be replaced without stopping audio
- State migration via optional `migrate()` function
- Engine handles scheduling and buffer management
- Zero-downtime graph updates

**🧵 Thread-safe**: Kernels are pure functions (given same input → same output)
- No shared mutable state between calls
- Safe for parallel execution across worker threads
- Deterministic behavior for testing

### Node Memory Model

State is represented as a simple **float array** with indexed parameters:

```c
// Oscillator memory layout
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

const NodeVTable OSC_VTABLE = {
    .process = osc_process,
    .init = osc_init,
    .migrate = osc_migrate
};
```

### Generic Node Creation

```c
// Create any node type with a simple helper
AudioNode* my_filter = create_generic_node(
    gb,                    // GraphBuilder
    my_filter_process,     // Process function
    FILTER_MEMORY_SIZE,    // Memory slots needed
    1,                     // Number of inputs  
    1,                     // Number of outputs
    "lowpass"              // Name
);

// Initialize parameters directly
float* memory = (float*)my_filter->state;
memory[CUTOFF_FREQ] = 1000.0f;
memory[RESONANCE] = 0.7f;
```

### AudioNode: High-Level Wrapper

The `AudioNode` provides a user-friendly interface over the kernel system:

```c
typedef struct AudioNode {
    uint64_t logical_id;     // Unique identifier for parameter targeting
    NodeVTable vtable;       // Contains the actual processing kernel
    void* state;             // Kernel's memory array (float*)
    
    // Connection tracking (build-time only) 
    struct AudioNode** inputs;
    struct AudioNode** outputs;
    // ... connection management fields
} AudioNode;
```

**Kernel ↔ AudioNode Relationship:**

```c
// 1. Create AudioNode with kernel
AudioNode* osc = create_oscillator(gb, 440.0f, "A4");
// Internally: osc->vtable = OSC_VTABLE, osc->state = float[OSC_MEMORY_SIZE]

// 2. Graph compilation converts AudioNode → RTNode
GraphState* graph = compile_graph(gb, 48000, 128, "my_graph");
// RTNode now contains the kernel for direct execution

// 3. Engine calls kernel directly
RTNode* node = &graph->nodes[i];
node->vtable.process(inputs, outputs, nframes, node->state);
// node->state points to the float array with indexed parameters
```

**The Two-Phase Design:**

1. **Build Phase**: `AudioNode` provides Web Audio-style API (`connect()`, `create_oscillator()`)
2. **Runtime Phase**: `RTNode` strips away build-time data, keeps only the kernel + state

This separation enables:
- **Easy graph construction** through intuitive AudioNode API
- **Maximum performance** by removing abstraction overhead at runtime
- **Memory efficiency** by discarding connection metadata after compilation

This kernel model enables AudioGraph to:
- **Scale processing** across multiple CPU cores
- **Live-edit graphs** by swapping kernels while audio runs  
- **Maintain real-time guarantees** through predictable execution
- **Support any DSP algorithm** via the simple function interface

## Parameter Updates

AudioGraph provides **lock-free parameter updates** that allow real-time control of node parameters without blocking the audio thread.

### SPSC Parameter Ring

Parameters are sent via a **Single Producer, Single Consumer** lock-free ring buffer:

```c
typedef struct {
    uint64_t idx;        // Parameter index (e.g., PARAM_SET_GAIN)
    uint64_t logical_id; // Target node identifier  
    float fvalue;        // New parameter value
} ParamMsg;

typedef struct ParamRing {
    ParamMsg buf[PARAM_RING_CAP];  // Fixed-size ring buffer
    _Atomic uint32_t head;         // Producer writes here
    _Atomic uint32_t tail;         // Consumer reads here
} ParamRing;
```

### Thread-Safe Parameter Flow

```
UI Thread                    Audio Thread
─────────────                ────────────
params_push() ──────────────→ apply_params()
   │                             │
   ├─ Write to ring buffer       ├─ Read from ring buffer  
   ├─ Update head pointer        ├─ Update node state
   └─ Never blocks               └─ Update tail pointer
```

### Usage Example

```c
// From UI/control thread (never blocks)
ParamMsg msg = {
    .idx = GAIN_VALUE,       // Direct memory index
    .logical_id = 0x3333,    // Target gain node
    .fvalue = 0.8f           // New gain value
};
params_push(graph->params, msg);

// In audio thread (before processing each block)
apply_params(graph);  // Reads all pending parameters
```

### Parameter Application

The audio thread processes parameters before each block:

```c
void apply_params(GraphState* g) {
    ParamMsg m;
    while(params_pop(g->params, &m)) {           // Drain all pending
        for(int i = 0; i < g->nodeCount; i++) {
            if(g->nodes[i].logical_id == m.logical_id) {
                if(g->nodes[i].state) {          // Only if node has memory
                    float* memory = (float*)g->nodes[i].state;
                    memory[m.idx] = m.fvalue;    // Direct indexed update
                }
            }
        }
    }
}
```

### Key Benefits

**🚫 No Locks**: Lock-free ring buffer never blocks either thread
- Audio thread maintains real-time guarantees
- UI thread never stalls waiting for audio

**⚡ Low Latency**: Parameters apply within one audio block
- Typical latency: 128 samples ≈ 2.7ms at 48kHz
- Immediate response for live performance

**🎯 Targeted Updates**: Each parameter targets specific nodes via `logical_id`
- Efficient lookup using stable node identifiers
- Multiple parameters can update simultaneously

**🔒 Thread Safe**: Atomic operations ensure consistency
- No torn reads/writes of parameter values
- Safe concurrent access from multiple threads

### Swift Integration

```swift
extension AudioGraphEngine {
    func setGain(nodeId: UInt64, value: Float) {
        let msg = ParamMsg(
            idx: GAIN_VALUE,         // Direct memory index
            logical_id: nodeId,
            fvalue: value
        )
        params_push(liveGraph.pointee.params, msg)
    }
    
    func setOscillatorFreq(nodeId: UInt64, freq: Float) {
        let msg = ParamMsg(
            idx: OSC_INC,           // Frequency increment parameter
            logical_id: nodeId,
            fvalue: freq / 48000.0  // Convert to phase increment
        )
        params_push(liveGraph.pointee.params, msg)
    }
}
```

This parameter system enables **smooth real-time control** without sacrificing audio thread performance or introducing dropouts.

## Building

```bash
make                    # Build the project
make debug             # Debug build with symbols
make run               # Build and run live demo
./audiograph compiled  # Run compiled graph demo
```

## C API Usage

### Basic Graph Building

```c
#include "graph_api.h"

// Create a graph builder
GraphBuilder* gb = create_graph_builder();

// Create nodes
AudioNode* osc = create_oscillator(gb, 440.0f, "A4");
AudioNode* gain = create_gain(gb, 0.5f, "volume");
AudioNode* out = create_mixer2(gb, "output");

// Connect nodes (Web Audio style)
connect(osc, gain);
connect(gain, out);

// Compile to runtime graph
GraphState* graph = compile_graph(gb, 48000, 128, "my_graph");
free_graph_builder(gb);
```

### Live Graph Editing

```c
#include "graph_engine.h"

// Create live graph
LiveGraph* lg = create_live_graph(16, 128, "live");

// Add nodes dynamically
int osc1 = live_add_oscillator(lg, 440.0f, "osc1");
int gain1 = live_add_gain(lg, 0.5f, "gain1");
int mixer = live_add_mixer2(lg, "mixer");

// Connect while audio is running
live_connect(lg, osc1, gain1);
live_connect(lg, gain1, mixer);

// Process audio (call from audio callback)
float output[128];
process_next_block(lg, output, 128);
```

### Multi-threaded Processing

```c
// Start worker pool
engine_start_workers(3);

// Process blocks in parallel
process_block_parallel(graph, 128);

// Clean shutdown
engine_stop_workers();
free_graph(graph);
```

## Swift Integration

### Compilation and Library Setup

First, compile AudioGraph as a static library:

```bash
# In the audiograph directory
make clean
gcc -std=c11 -O2 -Wall -Wextra -pthread -c graph_nodes.c -o graph_nodes.o
gcc -std=c11 -O2 -Wall -Wextra -pthread -c graph_engine.c -o graph_engine.o  
gcc -std=c11 -O2 -Wall -Wextra -pthread -c graph_api.c -o graph_api.o

# Create static library
ar rcs libaudiograph.a graph_nodes.o graph_engine.o graph_api.o

# Verify library contents
ar -t libaudiograph.a
```

### Swift Package Manager Integration

#### 1. Package.swift Configuration

```swift
// swift-tools-version: 5.7
import PackageDescription

let package = Package(
    name: "MyAudioApp",
    platforms: [.macOS(.v10_15), .iOS(.v13)],
    products: [
        .executable(name: "MyAudioApp", targets: ["MyAudioApp"])
    ],
    targets: [
        // C library target
        .target(
            name: "AudioGraphC",
            path: "Sources/AudioGraphC",
            sources: ["graph_nodes.c", "graph_engine.c", "graph_api.c"],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("include"),
                .define("_GNU_SOURCE"),
                .unsafeFlags(["-std=c11", "-pthread"])
            ]
        ),
        
        // Swift executable target
        .executableTarget(
            name: "MyAudioApp",
            dependencies: ["AudioGraphC"],
            path: "Sources/MyAudioApp"
        )
    ]
)
```

#### 2. Directory Structure

```
MyAudioApp/
├── Package.swift
├── Sources/
│   ├── AudioGraphC/
│   │   ├── include/
│   │   │   ├── graph_types.h
│   │   │   ├── graph_nodes.h
│   │   │   ├── graph_engine.h
│   │   │   └── graph_api.h
│   │   ├── graph_nodes.c
│   │   ├── graph_engine.c
│   │   └── graph_api.c
│   └── MyAudioApp/
│       └── main.swift
```

#### 3. Module Map (Optional)

If SPM auto-generation doesn't work, create `Sources/AudioGraphC/include/module.modulemap`:

```
module AudioGraphC {
    header "graph_types.h"
    header "graph_nodes.h" 
    header "graph_engine.h"
    header "graph_api.h"
    export *
}
```


### Building and Running

```bash
# Build the Swift project
swift build

# Run the executable  
swift run MyAudioApp

# Build in release mode
swift build -c release

# Run tests (if you have them)
swift test
```

### Build Troubleshooting

**Missing pthread symbols:**
```bash
# Add linker flags to Package.swift
.unsafeFlags(["-lpthread"])
```

**Header not found:**
```bash
# Verify header path in cSettings
.headerSearchPath("include")
```

**Module not found:**
```bash
# Check import statement matches target name
import AudioGraphC  // Must match target name in Package.swift
```

### Basic Setup

```swift
import AVFoundation
import AudioGraphC  // Your C module

class AudioGraphEngine {
    private var liveGraph: OpaquePointer?
    private let blockSize: Int32 = 512
    
    init() {
        liveGraph = create_live_graph(16, blockSize, "swift_graph")
    }
    
    deinit {
        // Clean up (add proper LiveGraph cleanup to C API)
    }
}
```

### Core Audio Integration

```swift
class AudioGraphSourceNode: AVAudioSourceNode {
    private let engine: AudioGraphEngine
    
    init(engine: AudioGraphEngine) {
        self.engine = engine
        
        let format = AVAudioFormat(
            standardFormatWithSampleRate: 48000, 
            channels: 1
        )!
        
        super.init(format: format) { [weak self] _, frames, outputData in
            return self?.renderBlock(frames: frames, outputData: outputData) ?? noErr
        }
    }
    
    private func renderBlock(frames: AVAudioFrameCount, outputData: AudioBufferList) -> OSStatus {
        let buffer = UnsafeMutableAudioBufferListPointer(outputData)[0]
        let output = buffer.mData?.assumingMemoryBound(to: Float.self)
        
        // Call your C function
        process_next_block(engine.liveGraph, output, Int32(frames))
        
        return noErr
    }
}
```

### Live Graph Manipulation

```swift
extension AudioGraphEngine {
    func addOscillator(frequency: Float, name: String) -> Int32 {
        return live_add_oscillator(liveGraph, frequency, name)
    }
    
    func addGain(value: Float, name: String) -> Int32 {
        return live_add_gain(liveGraph, value, name)
    }
    
    func connect(source: Int32, destination: Int32) -> Bool {
        return live_connect(liveGraph, source, destination)
    }
    
    func disconnect(source: Int32, destination: Int32) -> Bool {
        return live_disconnect(liveGraph, source, destination)
    }
}
```

### Complete Audio App Example

```swift
import AVFoundation

class AudioApp {
    private let audioEngine = AVAudioEngine()
    private let graphEngine = AudioGraphEngine()
    private var sourceNode: AudioGraphSourceNode!
    
    func start() {
        // Set up audio graph
        let osc1 = graphEngine.addOscillator(frequency: 440, name: "A4")
        let osc2 = graphEngine.addOscillator(frequency: 660, name: "E5")
        let gain1 = graphEngine.addGain(value: 0.5, name: "vol1")
        let gain2 = graphEngine.addGain(value: 0.3, name: "vol2")
        let mixer = graphEngine.addMixer2(name: "output")
        
        // Connect nodes
        _ = graphEngine.connect(source: osc1, destination: gain1)
        _ = graphEngine.connect(source: osc2, destination: gain2)
        _ = graphEngine.connect(source: gain1, destination: mixer)
        _ = graphEngine.connect(source: gain2, destination: mixer)
        
        // Set up Core Audio
        sourceNode = AudioGraphSourceNode(engine: graphEngine)
        audioEngine.attach(sourceNode)
        audioEngine.connect(sourceNode, to: audioEngine.outputNode, format: sourceNode.outputFormat(forBus: 0))
        
        // Start audio
        try? audioEngine.start()
    }
    
    func liveEdit() {
        // Disconnect and reconnect while audio is playing!
        let osc1 = 0, gain1 = 2, mixer = 4
        _ = graphEngine.disconnect(source: osc1, destination: gain1)
        _ = graphEngine.connect(source: osc1, destination: mixer) // Bypass gain
    }
}
```

## Threading Model

- **Audio Thread**: Calls `process_next_block()` (real-time safe)
- **Main Thread**: Graph building, live editing operations
- **Worker Pool**: Parallel node processing (optional)

⚠️ **Real-time Safety**: Only `process_next_block()` is safe to call from audio threads. All other operations should be performed on background threads.

## Performance

- **Latency**: Determined by your block size (e.g., 128 samples ≈ 2.7ms at 48kHz)
- **Throughput**: Scales with CPU cores when using worker pool
- **Memory**: Lock-free algorithms, minimal allocation in audio path

## License

GPL v3 - See LICENSE file for details.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Add tests for new functionality
4. Submit a pull request

## Examples

See `main.c` for complete examples of both compiled graphs with hot-swapping and live graph editing.