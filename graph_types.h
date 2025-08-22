#ifndef GRAPH_TYPES_H
#define GRAPH_TYPES_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

// ===================== Constants =====================
#define MAX_IO 8

// ===================== Helpers =====================
static inline void* alloc_aligned(size_t alignment, size_t size) {
    void* p = NULL;
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}

static inline uint64_t nsec_now(void) {
    struct timespec ts; 
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// ===================== Kernel ABI =====================
typedef void (*KernelFn)(float* const* in, float* const* out, int nframes, void* state);
typedef void (*InitFn)(void* state, int sampleRate, int maxBlock);
typedef void (*ResetFn)(void* state);
typedef void (*MigrateFn)(void* newState, const void* oldState);

typedef struct {
    KernelFn  process;
    InitFn    init;      // optional
    ResetFn   reset;     // optional
    MigrateFn migrate;   // optional: copy persistent state on graph swap
} NodeVTable;

// ===================== Parameter Mailbox =====================
typedef enum { 
    PARAM_SET_GAIN = 1 
} ParamKind;

typedef struct {
    ParamKind kind;
    uint64_t  logical_id;  // target node
    float     fvalue;      // e.g., new gain
} ParamMsg;

#define PARAM_RING_CAP  256

typedef struct ParamRing {
    ParamMsg  buf[PARAM_RING_CAP];
    _Atomic uint32_t head; // producer writes
    _Atomic uint32_t tail; // consumer reads
} ParamRing;

// Parameter ring operations
static inline bool params_push(ParamRing* r, ParamMsg m) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    if ((h - t) >= PARAM_RING_CAP) return false; // full
    r->buf[h % PARAM_RING_CAP] = m;
    atomic_store_explicit(&r->head, h+1, memory_order_release);
    return true;
}

static inline bool params_pop(ParamRing* r, ParamMsg* out) {
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (t == h) return false; // empty
    *out = r->buf[t % PARAM_RING_CAP];
    atomic_store_explicit(&r->tail, t+1, memory_order_release);
    return true;
}

#endif // GRAPH_TYPES_H