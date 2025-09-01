# AudioGraph Swift Integration Guide

This guide shows how to integrate the audiograph dylib into your Swift project for real-time audio processing.

## Building the Dynamic Library

### 1. Compile the Library
```bash
# Build optimized release version
make lib-release

# This creates: libaudiograph.dylib
```

### 2. Copy Library Files to Your Swift Project
Copy these files to your Swift project:
- `libaudiograph.dylib` - The dynamic library
- `audiograph_swift.h` - Swift-compatible header file
- All `.h` files (`graph_types.h`, `graph_engine.h`, etc.) - Required for compilation

## Swift Project Setup

### 1. Add Library to Xcode Project
1. Drag `libaudiograph.dylib` into your Xcode project
2. Add to "Frameworks and Libraries" in your target settings
3. Set "Embed" to "Do Not Embed" (for dylib)

### 2. Create Bridging Header
Create a bridging header file (e.g., `YourProject-Bridging-Header.h`):
```c
#import "audiograph_swift.h"
```

### 3. Configure Build Settings
In your target build settings:
- Set "Objective-C Bridging Header" to your bridging header path
- Add library search path: `$(PROJECT_DIR)/path/to/dylib`
- Add header search path: `$(PROJECT_DIR)/path/to/headers`

### 4. Set Runtime Library Path
Add this to your target's "Runpath Search Paths":
```
@executable_path
@loader_path
```

## Swift Usage Example

```swift
import Foundation

class AudioGraphManager {
    private var liveGraph: OpaquePointer?
    private let blockSize: Int32 = 128
    private let sampleRate: Int32 = 48000
    
    init() {
        // Initialize the engine once
        initialize_engine(blockSize, sampleRate)
        
        // Create live graph
        liveGraph = create_live_graph(16, blockSize, "swift_graph")
        
        // Start worker threads
        engine_start_workers(4)
    }
    
    deinit {
        engine_stop_workers()
        if let graph = liveGraph {
            destroy_live_graph(graph)
        }
    }
    
    func createOscillator(frequency: Float, name: String) -> Int32 {
        guard let graph = liveGraph else { return -1 }
        return live_add_oscillator(graph, frequency, name)
    }
    
    func createGain(value: Float, name: String) -> Int32 {
        guard let graph = liveGraph else { return -1 }
        return live_add_gain(graph, value, name)
    }
    
    func createMixer(name: String) -> Int32 {
        guard let graph = liveGraph else { return -1 }
        return live_add_mixer2(graph, name)
    }
    
    func connect(sourceNode: Int32, sourcePort: Int32, 
                 destNode: Int32, destPort: Int32) -> Bool {
        guard let graph = liveGraph else { return false }
        return connect(graph, sourceNode, sourcePort, destNode, destPort)
    }
    
    func disconnect(sourceNode: Int32, sourcePort: Int32,
                    destNode: Int32, destPort: Int32) -> Bool {
        guard let graph = liveGraph else { return false }
        return disconnect(graph, sourceNode, sourcePort, destNode, destPort)
    }
    
    func processAudioBlock() -> [Float] {
        guard let graph = liveGraph else { return [] }
        
        var outputBuffer = [Float](repeating: 0.0, count: Int(blockSize))
        outputBuffer.withUnsafeMutableBufferPointer { buffer in
            process_next_block(graph, buffer.baseAddress!, blockSize)
        }
        
        return outputBuffer
    }
    
    func updateParameter(nodeId: Int32, paramIndex: Int, value: Float) {
        guard let graph = liveGraph else { return }
        
        // Access the parameter ring buffer
        let paramMsg = ParamMsg(
            idx: UInt64(paramIndex),
            logical_id: UInt64(nodeId),
            fvalue: value
        )
        
        // Note: You'll need to access the params field of LiveGraph
        // This might require a helper function in C
        _ = params_push(graph.pointee.params, paramMsg)
    }
}
```

## Real-time Audio Integration

### With AVAudioEngine
```swift
import AVFoundation

class AudioGraphAVEngine {
    private let audioGraphManager = AudioGraphManager()
    private let audioEngine = AVAudioEngine()
    
    func startAudio() throws {
        let mainMixer = audioEngine.mainMixerNode
        let outputNode = audioEngine.outputNode
        let format = outputNode.inputFormat(forBus: 0)
        
        // Install a tap to process audio with audiograph
        mainMixer.installTap(onBus: 0, bufferSize: 128, format: format) { buffer, time in
            let audioBuffer = self.audioGraphManager.processAudioBlock()
            
            // Copy audiograph output to AVAudioPCMBuffer
            let frameLength = buffer.frameLength
            if let channelData = buffer.floatChannelData {
                for frame in 0..<Int(frameLength) {
                    if frame < audioBuffer.count {
                        channelData[0][frame] = audioBuffer[frame]
                    }
                }
            }
        }
        
        try audioEngine.start()
    }
    
    func stopAudio() {
        audioEngine.stop()
    }
}
```

### With Audio Unit
For lower-level integration, you can use the audiograph in an Audio Unit render callback:

```swift
let renderCallback: AURenderCallback = { (inRefCon, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData) -> OSStatus in
    
    guard let audioGraphManager = inRefCon?.assumingMemoryBound(to: AudioGraphManager.self).pointee else {
        return noErr
    }
    
    let audioBuffer = audioGraphManager.processAudioBlock()
    
    // Copy to output buffer
    guard let ioData = ioData,
          let buffers = ioData.pointee.mBuffers.mData?.assumingMemoryBound(to: Float.self) else {
        return noErr
    }
    
    for i in 0..<min(Int(inNumberFrames), audioBuffer.count) {
        buffers[i] = audioBuffer[i]
    }
    
    return noErr
}
```

## Memory Management Notes

### Important Considerations:
1. **Thread Safety**: Only `process_next_block()` and `params_push()` are real-time safe
2. **Memory Allocation**: All graph edits (`add_node`, `connect`, etc.) should be done from the main thread
3. **Lifecycle**: Always call `engine_stop_workers()` before destroying the live graph
4. **Parameter Updates**: Use the parameter ring buffer for real-time safe parameter changes

### Recommended Pattern:
```swift
// Main thread: Setup and graph editing
let oscId = audioGraphManager.createOscillator(frequency: 440.0, name: "A4")
let gainId = audioGraphManager.createGain(value: 0.5, name: "volume")
_ = audioGraphManager.connect(sourceNode: oscId, sourcePort: 0, 
                             destNode: gainId, destPort: 0)

// Audio thread: Only process audio
let samples = audioGraphManager.processAudioBlock()

// Any thread: Parameter updates (lock-free)
audioGraphManager.updateParameter(nodeId: gainId, paramIndex: 0, value: 0.8)
```

## Building for Distribution

### Static vs Dynamic Linking
- **Dynamic Library (.dylib)**: Easier to update, smaller app binary
- **Static Library (.a)**: Single binary, no external dependencies

### For App Store Distribution
Consider creating a static library version:
```bash
# Create static library
ar rcs libaudiograph.a graph_nodes.o graph_engine.o graph_api.o graph_edit.o
```

## Troubleshooting

### Common Issues:
1. **Symbol not found**: Ensure bridging header includes `audiograph_swift.h`
2. **Runtime crashes**: Check that `initialize_engine()` is called before creating graphs
3. **Audio glitches**: Ensure audio processing happens only in `process_next_block()`
4. **Memory leaks**: Always pair `create_live_graph()` with `destroy_live_graph()`

### Debug Build:
For debugging, use the debug version:
```bash
make clean && make lib debug
```