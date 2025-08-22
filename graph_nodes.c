#include "graph_nodes.h"

// ===================== Oscillator Implementation =====================

void osc_init(void* st, int sr, int maxBlock) {
    (void)sr; 
    (void)maxBlock; 
    OscState* s = (OscState*)st; 
    s->phase = 0.f;
}

void osc_process(float* const* in, float* const* out, int n, void* st) {
    (void)in; 
    OscState* s = (OscState*)st; 
    float* y = out[0];
    for(int i = 0; i < n; i++) {
        y[i] = 2.0f * s->phase - 1.0f; 
        s->phase += s->inc; 
        if(s->phase >= 1.f) s->phase -= 1.f;
    }
}

void osc_migrate(void* newState, const void* oldState) {
    const OscState* o = (const OscState*)oldState; 
    OscState* n = (OscState*)newState; 
    n->phase = o->phase;
}

// ===================== Gain Implementation =====================

void gain_process(float* const* in, float* const* out, int n, void* st) {
    float g = ((GainState*)st)->g; 
    const float* a = in[0]; 
    float* y = out[0]; 
    for(int i = 0; i < n; i++) y[i] = a[i] * g;
}

// ===================== Mixer Implementations =====================

void mix2_process(float* const* in, float* const* out, int n, void* st) {
    (void)st; 
    const float* a = in[0]; 
    const float* b = in[1]; 
    float* y = out[0]; 
    for(int i = 0; i < n; i++) y[i] = a[i] + b[i];
}

void mix3_process(float* const* in, float* const* out, int n, void* st) {
    (void)st; 
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