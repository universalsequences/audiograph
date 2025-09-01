import AudioGraph
import AVFoundation
import Foundation

// Global audio graph manager for use in render callback
class AudioGraphManager {
    private var liveGraph: UnsafeMutablePointer<LiveGraph>?
    private let blockSize: Int32 = 512
    private var audioBuffer = [Float]()
    
    init() {
        setupAudioGraph()
    }
    
    private func setupAudioGraph() {
        // Initialize engine with matching sample rate
        initialize_engine(blockSize, 44100)
        
        guard let lg = create_live_graph(32, blockSize, "swift_av_graph") else {
            print("✗ Failed to create live graph")
            return
        }
        
        liveGraph = lg
        audioBuffer = [Float](repeating: 0.0, count: Int(blockSize))
        
        // Start worker threads
        engine_start_workers(2)
        
        // Create multiple oscillators for a chord
        let frequencies: [Float] = [261.63, 329.63, 392.00, 523.25] // C major chord
        let gainPerOsc: Float = 0.15  // Adjusted for 4 oscillators
        
        var oscNodes: [Int32] = []
        var gainNodes: [Int32] = []
        
        // Create oscillators and individual gain controls
        for (i, freq) in frequencies.enumerated() {
            let osc = live_add_oscillator(lg, freq, "osc_\(i)")
            let gain = live_add_gain(lg, gainPerOsc, "gain_\(i)")
            oscNodes.append(osc)
            gainNodes.append(gain)
            
            // Connect osc -> gain
            _ = connect(lg, osc, 0, gain, 0)
        }
        
        // Create master mixer and master gain
        let mixer = live_add_mixer8(lg, "master_mix")
        let masterGain = live_add_gain(lg, 0.5, "master_vol")
        
        // Connect all gains to mixer inputs
        for (i, gainNode) in gainNodes.enumerated() {
            _ = connect(lg, gainNode, 0, mixer, Int32(i))
        }
        
        // Connect mixer -> master gain -> DAC
        _ = connect(lg, mixer, 0, masterGain, 0)
        _ = connect(lg, masterGain, 0, lg.pointee.dac_node_id, 0)
        
        print("✓ Created audio graph: 4 oscillators -> individual gains -> mixer -> master gain -> output")
        print("✓ Frequencies: \(frequencies.map { "\($0)Hz" }.joined(separator: ", "))")
    }
    
    func renderAudio(frameCount: UInt32, audioBufferList: UnsafeMutablePointer<AudioBufferList>) -> OSStatus {
        guard let lg = liveGraph else { return kAudioUnitErr_Uninitialized }
        
        let ablPointer = UnsafeMutableAudioBufferListPointer(audioBufferList)
        guard let leftBuffer = ablPointer[0].mData?.assumingMemoryBound(to: Float.self),
              let rightBuffer = ablPointer[1].mData?.assumingMemoryBound(to: Float.self) else {
            return kAudioUnitErr_InvalidParameter
        }
        
        var framesProcessed: UInt32 = 0
        
        // Process in chunks of our block size
        while framesProcessed < frameCount {
            let framesToProcess = min(UInt32(blockSize), frameCount - framesProcessed)
            
            // Get audio from audiograph (mono output)
            audioBuffer.withUnsafeMutableBufferPointer { bufferPtr in
                process_next_block(lg, bufferPtr.baseAddress!, Int32(framesToProcess))
            }
            
            // Copy mono signal to both stereo channels
            for i in 0..<Int(framesToProcess) {
                let sample = audioBuffer[i]
                leftBuffer[Int(framesProcessed) + i] = sample
                rightBuffer[Int(framesProcessed) + i] = sample
            }
            
            framesProcessed += framesToProcess
        }
        
        return noErr
    }
    
    deinit {
        engine_stop_workers()
        if let lg = liveGraph {
            destroy_live_graph(lg)
        }
    }
}

class AudioGraphSourceNode: AVAudioSourceNode {
    private let audioGraphManager = AudioGraphManager()
    
    init() {
        // Initialize with stereo format at 44.1kHz (standard audio)  
        let format = AVAudioFormat(standardFormatWithSampleRate: 44100, channels: 2)!
        
        super.init(format: format) { [weak audioGraphManager] _, _, frameCount, audioBufferList -> OSStatus in
            return audioGraphManager?.renderAudio(frameCount: frameCount, audioBufferList: audioBufferList) ?? kAudioUnitErr_Uninitialized
        }
    }
}

func main() {
    print("AudioGraph + AVAudioEngine Real-time Example")
    print("===========================================")
    
    let audioEngine = AVAudioEngine()
    let sourceNode = AudioGraphSourceNode()
    let mainMixer = audioEngine.mainMixerNode
    
    // Attach our custom source node
    audioEngine.attach(sourceNode)
    
    // Connect source -> mixer -> output
    audioEngine.connect(sourceNode, to: mainMixer, format: sourceNode.outputFormat(forBus: 0))
    audioEngine.connect(mainMixer, to: audioEngine.outputNode, format: nil)
    
    do {
        try audioEngine.start()
        print("✓ Audio engine started - you should hear a C major chord!")
        print("✓ Playing for 5 seconds...")
        
        // Play for 5 seconds
        Thread.sleep(forTimeInterval: 5.0)
        
        audioEngine.stop()
        print("✓ Audio stopped")
        
    } catch {
        print("✗ Audio engine error: \(error)")
    }
    
    print("\n✓ Real-time audio integration successful!")
}

main()