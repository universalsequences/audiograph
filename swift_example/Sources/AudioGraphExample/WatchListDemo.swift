// WatchListDemo.swift - Example usage of watch list API from Swift

import Foundation

// Example of how to use the watchlist API from Swift
// Note: This assumes the audiograph_swift.h header is included in a bridging header

class WatchListDemo {
    private var liveGraph: OpaquePointer?
    
    init() {
        // Initialize the engine (typically done once at app startup)
        initialize_engine(128, 48000)
        engine_start_workers(2)
        
        // Create a live graph for real-time audio processing
        liveGraph = create_live_graph(16, 128, "SwiftWatchListDemo", 1)
    }
    
    deinit {
        if let lg = liveGraph {
            destroy_live_graph(lg)
        }
        engine_stop_workers()
    }
    
    func demonstrateWatchList() {
        guard let lg = liveGraph else { 
            print("Error: Live graph not initialized")
            return 
        }
        
        print("=== Swift Watch List Demo ===")
        
        // Create some audio nodes
        let oscId = live_add_oscillator(lg, 440.0, "demo_osc")
        let gainId = live_add_gain(lg, 0.7, "demo_gain")
        
        print("Created oscillator node: \(oscId)")
        print("Created gain node: \(gainId)")
        
        // Add nodes to watch list
        let result1 = add_node_to_watchlist(lg, oscId)
        let result2 = add_node_to_watchlist(lg, gainId)
        
        print("Added oscillator to watchlist: \(result1)")
        print("Added gain to watchlist: \(result2)")
        
        // Connect the nodes: oscillator -> gain -> DAC
        graph_connect(lg, oscId, 0, gainId, 0)
        graph_connect(lg, gainId, 0, 0, 0)
        
        // Process a few audio blocks
        let bufferSize = 128
        let outputBuffer = UnsafeMutablePointer<Float>.allocate(capacity: bufferSize)
        defer { outputBuffer.deallocate() }
        
        for blockNum in 1...3 {
            print("\\n--- Processing block \(blockNum) ---")
            
            process_next_block(lg, outputBuffer, Int32(bufferSize))
            
            // Get node states
            var oscStateSize: Int = 0
            var gainStateSize: Int = 0
            
            if let oscState = get_node_state(lg, oscId, &oscStateSize) {
                print("Oscillator state retrieved: \(oscStateSize) bytes")
                
                // Cast to float pointer to read state data
                let oscFloats = oscState.assumingMemoryBound(to: Float.self)
                if oscStateSize >= MemoryLayout<Float>.size {
                    print("  First float value: \(oscFloats[0])")
                }
                if oscStateSize >= 2 * MemoryLayout<Float>.size {
                    print("  Second float value: \(oscFloats[1])")
                }
                
                // Important: Free the allocated memory
                free(oscState)
            }
            
            if let gainState = get_node_state(lg, gainId, &gainStateSize) {
                print("Gain state retrieved: \(gainStateSize) bytes")
                
                let gainFloats = gainState.assumingMemoryBound(to: Float.self)
                if gainStateSize >= MemoryLayout<Float>.size {
                    print("  Gain value: \(gainFloats[0])")
                }
                
                free(gainState)
            }
            
            // Calculate output signal level
            var maxOutput: Float = 0.0
            for i in 0..<bufferSize {
                let absValue = abs(outputBuffer[i])
                if absValue > maxOutput {
                    maxOutput = absValue
                }
            }
            print("Output peak level: \(maxOutput)")
        }
        
        // Remove oscillator from watch list
        print("\\nRemoving oscillator from watchlist...")
        let removed = remove_node_from_watchlist(lg, oscId)
        print("Removal result: \(removed)")
        
        // Process one more block
        process_next_block(lg, outputBuffer, Int32(bufferSize))
        
        // Try to get states again
        let oscStateAfter = get_node_state(lg, oscId, nil)
        let gainStateAfter = get_node_state(lg, gainId, nil)
        
        print("Oscillator state after removal: \(oscStateAfter != nil ? "available" : "nil")")
        print("Gain state after removal: \(gainStateAfter != nil ? "available" : "nil")")
        
        // Clean up any remaining state
        if let gainState = gainStateAfter {
            free(gainState)
        }
        
        print("\\n✓ Swift Watch List Demo completed successfully!")
    }
}

// Example usage:
// let demo = WatchListDemo()
// demo.demonstrateWatchList()