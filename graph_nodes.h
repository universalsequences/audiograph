#ifndef GRAPH_NODES_H
#define GRAPH_NODES_H

#include "graph_types.h"

// ===================== Node Memory Layout =====================

// Oscillator memory layout
#define OSC_MEMORY_SIZE 2
#define OSC_PHASE 0
#define OSC_INC   1

// Gain memory layout  
#define GAIN_MEMORY_SIZE 1
#define GAIN_VALUE 0

// Mixer has no state
#define MIX_MEMORY_SIZE 0

// ===================== Node Processing Functions =====================

// Oscillator functions
void osc_init(void* memory, int sr, int maxBlock);
void osc_process(float* const* in, float* const* out, int n, void* memory);
void osc_migrate(void* newMemory, const void* oldMemory);

// Gain function
void gain_process(float* const* in, float* const* out, int n, void* memory);

// Mixer functions
void mix2_process(float* const* in, float* const* out, int n, void* memory);
void mix3_process(float* const* in, float* const* out, int n, void* memory);
void mix8_process(float* const* in, float* const* out, int n, void* memory);

// DAC function (Digital-to-Analog Converter - final output sink)
void dac_process(float* const* in, float* const* out, int n, void* memory);

// ===================== Node VTables =====================

extern const NodeVTable OSC_VTABLE;
extern const NodeVTable GAIN_VTABLE;
extern const NodeVTable MIX2_VTABLE;
extern const NodeVTable MIX8_VTABLE;
extern const NodeVTable DAC_VTABLE;


#endif // GRAPH_NODES_H