#ifndef GRAPH_NODES_H
#define GRAPH_NODES_H

#include "graph_types.h"

// ===================== Node State Structures =====================

typedef struct {
    float phase;
    float inc;
} OscState;

typedef struct {
    float g;
} GainState;

// ===================== Node Processing Functions =====================

// Oscillator functions
void osc_init(void* st, int sr, int maxBlock);
void osc_process(float* const* in, float* const* out, int n, void* st);
void osc_migrate(void* newState, const void* oldState);

// Gain function
void gain_process(float* const* in, float* const* out, int n, void* st);

// Mixer functions
void mix2_process(float* const* in, float* const* out, int n, void* st);
void mix3_process(float* const* in, float* const* out, int n, void* st);

// ===================== Node VTables =====================

extern const NodeVTable OSC_VTABLE;
extern const NodeVTable GAIN_VTABLE;
extern const NodeVTable MIX2_VTABLE;

#endif // GRAPH_NODES_H