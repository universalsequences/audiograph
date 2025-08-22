#include "graph_nodes.h"

// ===================== Oscillator Implementation =====================

void osc_init(void* memory, int sr, int maxBlock) {
    (void)sr; 
    (void)maxBlock; 
    float* mem = (float*)memory;
    mem[OSC_PHASE] = 0.0f;
    // OSC_INC should be set during node creation
}

void osc_process(float* const* in, float* const* out, int n, void* memory) {
    (void)in; 
    float* mem = (float*)memory;
    float* y = out[0];
    
    // DEBUG: Track frame processing for race condition detection
    static _Atomic int debug_block_counter = 0;
    static _Atomic int debug_frame_counts[64] = {0}; // Max 64 oscillators
    static int debug_osc_index = 0; // This is not thread-safe, but good enough for debugging
    
    printf("    [OSC_DEBUG] Processing %d frames, initial phase=%.6f, inc=%.6f\n", 
           n, mem[OSC_PHASE], mem[OSC_INC]);
    
    for(int i = 0; i < n; i++) {
        y[i] = 2.0f * mem[OSC_PHASE] - 1.0f; 
        mem[OSC_PHASE] += mem[OSC_INC]; 
        if(mem[OSC_PHASE] >= 1.f) mem[OSC_PHASE] -= 1.f;
    }
    
    printf("    [OSC_DEBUG] Completed %d frames, final phase=%.6f, first_sample=%.6f\n", 
           n, mem[OSC_PHASE], y[0]);
}

void osc_migrate(void* newMemory, const void* oldMemory) {
    const float* oldMem = (const float*)oldMemory; 
    float* newMem = (float*)newMemory; 
    newMem[OSC_PHASE] = oldMem[OSC_PHASE];
    // OSC_INC typically doesn't need migration (set during creation)
}

// ===================== Gain Implementation =====================

void gain_process(float* const* in, float* const* out, int n, void* memory) {
    float* mem = (float*)memory;
    float gain = mem[GAIN_VALUE]; 
    const float* a = in[0]; 
    float* y = out[0]; 
    for(int i = 0; i < n; i++) y[i] = a[i] * gain;
}

// ===================== Mixer Implementations =====================

void mix2_process(float* const* in, float* const* out, int n, void* memory) {
    (void)memory; // Mixers have no state
    const float* a = in[0]; 
    const float* b = in[1]; 
    float* y = out[0]; 
    for(int i = 0; i < n; i++) y[i] = a[i] + b[i];
}

void mix3_process(float* const* in, float* const* out, int n, void* memory) {
    (void)memory; // Mixers have no state
    const float* a = in[0]; 
    const float* b = in[1]; 
    const float* c = in[2]; 
    float* y = out[0];
    for(int i = 0; i < n; i++) y[i] = a[i] + b[i] + c[i];
}

void mix8_process(float* const* in, float* const* out, int n, void* memory) {
    (void)memory; // Mixers have no state
    float* y = out[0];
    
    // Sum all 8 inputs
    for(int i = 0; i < n; i++) {
        y[i] = in[0][i] + in[1][i] + in[2][i] + in[3][i] + 
               in[4][i] + in[5][i] + in[6][i] + in[7][i];
    }
}

// ===================== Node VTables =====================

const NodeVTable OSC_VTABLE = {
    .process = osc_process,
    .init = osc_init,
    .reset = NULL,
    .migrate = osc_migrate
};

const NodeVTable GAIN_VTABLE = {
    .process = gain_process,
    .init = NULL,
    .reset = NULL,
    .migrate = NULL
};

const NodeVTable MIX2_VTABLE = {
    .process = mix2_process,
    .init = NULL,
    .reset = NULL,
    .migrate = NULL
};

