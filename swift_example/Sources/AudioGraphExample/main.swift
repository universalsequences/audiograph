import AVFoundation
import AudioGraph
import Foundation

// Simple kernel manager that compiles and loads our custom C code
class CustomKernelManager {
    private var dylibHandle: UnsafeMutableRawPointer?
    private var kernelFn: (@convention(c) (UnsafePointer<UnsafeMutablePointer<Float>?>?, UnsafePointer<UnsafeMutablePointer<Float>?>?, Int32, UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void)?
    
    private let kernelSource = """
    #include <stdio.h>
    
    // Simple kernel that outputs constant 0.5 on all channels
    void constant_kernel(float *const *in, float *const *out, int nframes, void *state, void *buffers) {
        (void)buffers;
        printf("=== CUSTOM KERNEL CALLED ===\\n");
        printf("in=%p, out=%p, nframes=%d, state=%p\\n", (void*)in, (void*)out, nframes, state);
        
        // Safety checks
        if (!out || !out[0]) {
            printf("ERROR: Invalid output buffer\\n");
            return;
        }
        
        if (nframes <= 0 || nframes > 8192) {
            printf("ERROR: Invalid nframes=%d\\n", nframes);
            return;
        }
        
        printf("SUCCESS: Writing constant 0.5 to %d frames\\n", nframes);
        
        // Output constant 0.5 to all frames
        for (int i = 0; i < nframes; i++) {
            out[0][i] = 0.5f;
        }
        
        printf("SUCCESS: constant_kernel completed\\n");
    }
    """
    
    func compileAndLoad() throws {
        let tmpDir = FileManager.default.temporaryDirectory
        let timestamp = String(Int(Date().timeIntervalSince1970 * 1000))
        let cFile = tmpDir.appendingPathComponent("kernel_\(timestamp).c")
        let dylibFile = tmpDir.appendingPathComponent("libkernel_\(timestamp).dylib")
        
        try kernelSource.write(to: cFile, atomically: true, encoding: .utf8)
        
        let compile = Process()
        compile.launchPath = "/usr/bin/clang"
        let arguments = [
            "-O3", "-march=armv8-a", "-fPIC", "-shared",
            "-framework", "Accelerate",
            "-std=c11",
            "-x", "c",
            "-o", dylibFile.path, cFile.path,
        ]
        
        print("🔧 Compiling kernel: clang \(arguments.joined(separator: " "))")
        
        let errorPipe = Pipe()
        compile.standardError = errorPipe
        compile.arguments = arguments
        compile.launch()
        compile.waitUntilExit()
        
        let errorData = errorPipe.fileHandleForReading.readDataToEndOfFile()
        let errorOutput = String(data: errorData, encoding: .utf8) ?? ""
        
        if !errorOutput.isEmpty {
            print("⚠️ CLANG STDERR OUTPUT:")
            print(errorOutput)
        }
        
        guard compile.terminationStatus == 0 else {
            throw NSError(domain: "CompileError", code: 1, userInfo: [NSLocalizedDescriptionKey: "Failed to compile kernel: \(errorOutput)"])
        }
        
        dylibHandle = dlopen(dylibFile.path, RTLD_NOW)
        guard let handle = dylibHandle else {
            let error = String(cString: dlerror())
            throw NSError(domain: "DLError", code: 2, userInfo: [NSLocalizedDescriptionKey: "Failed to load .dylib: \(error)"])
        }
        
        print("🔍 Looking for constant_kernel symbol...")
        guard let sym = dlsym(handle, "constant_kernel") else {
            let error = String(cString: dlerror())
            throw NSError(domain: "DLError", code: 3, userInfo: [NSLocalizedDescriptionKey: "Symbol constant_kernel not found: \(error)"])
        }
        
        kernelFn = unsafeBitCast(sym, to: (@convention(c) (UnsafePointer<UnsafeMutablePointer<Float>?>?, UnsafePointer<UnsafeMutablePointer<Float>?>?, Int32, UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void).self)
        
        print("✅ Custom kernel compiled and loaded successfully!")
    }
    
    func getKernelFunction() -> (@convention(c) (UnsafePointer<UnsafeMutablePointer<Float>?>?, UnsafePointer<UnsafeMutablePointer<Float>?>?, Int32, UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void)? {
        return kernelFn
    }
    
    deinit {
        if let handle = dylibHandle {
            dlclose(handle)
        }
    }
}

// Simple audio graph manager for testing custom kernel
class AudioGraphManager {
    private var liveGraph: UnsafeMutablePointer<LiveGraph>?
    private let blockSize: Int32 = 512
    private var audioBuffer = [Float]()
    private let kernelManager = CustomKernelManager()

    init() {
        setupAudioGraph()
    }

    private func setupAudioGraph() {
        // Initialize engine with matching sample rate
        initialize_engine(blockSize, 44100)

        guard let lg = create_live_graph(32, blockSize, "swift_kernel_test", 1) else {
            print("✗ Failed to create live graph")
            return
        }

        liveGraph = lg
        audioBuffer = [Float](repeating: 0.0, count: Int(blockSize))

        // Start worker threads
        engine_start_workers(4)

        // Compile and load our custom kernel
        do {
            try kernelManager.compileAndLoad()
        } catch {
            print("✗ Failed to compile/load kernel: \(error)")
            return
        }
        
        guard let kernelFn = kernelManager.getKernelFunction() else {
            print("✗ Failed to get kernel function")
            return
        }
        
        // Create a custom node with our kernel
        // Note: We need to create a NodeVTable and pass it to add_node
        let vtable = NodeVTable(
            process: kernelFn,
            init: nil,
            reset: nil,
            migrate: nil,
            begin_event_slice: nil,
            schedule_event: nil
        )
        
        let customNode = add_node(lg, vtable, 0, "custom_kernel", 0, 1, nil, 0)
        
        // Connect custom node directly to DAC
        _ = graph_connect(lg, customNode, 0, lg.pointee.dac_node_id, 0)
        
        print("✓ Created simple audio graph: custom_kernel -> DAC")
    }

    func testProcessNextBlock() {
        guard let lg = liveGraph else {
            print("✗ No live graph available")
            return
        }
        
        print("\n🧪 Testing process_next_block with custom kernel...")
        
        // Process a few blocks and print the results
        for blockNum in 0..<3 {
            print("\n--- Block \(blockNum + 1) ---")
            
            audioBuffer.withUnsafeMutableBufferPointer { bufferPtr in
                process_next_block(lg, bufferPtr.baseAddress!, blockSize)
            }
            
            // Print first few samples to verify our kernel is working
            print("First 8 samples: \(Array(audioBuffer.prefix(8)))")
            
            // Check if all samples are 0.5 as expected
            let allCorrect = audioBuffer.allSatisfy { $0 == 0.5 }
            print("All samples = 0.5: \(allCorrect ? "✅ YES" : "❌ NO")")
        }
    }

    deinit {
        engine_stop_workers()
        if let lg = liveGraph {
            destroy_live_graph(lg)
        }
    }
}

func main() {
    print("AudioGraph Custom Kernel Test")
    print("=============================")

    let manager = AudioGraphManager()
    
    // Test the custom kernel without AVAudioEngine
    manager.testProcessNextBlock()
    
    print("\n✅ Custom kernel test completed!")
}

main()
