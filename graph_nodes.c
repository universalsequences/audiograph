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
    
    for(int i = 0; i < n; i++) {
        y[i] = 2.0f * mem[OSC_PHASE] - 1.0f; 
        mem[OSC_PHASE] += mem[OSC_INC]; 
        if(mem[OSC_PHASE] >= 1.f) mem[OSC_PHASE] -= 1.f;
    }
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

