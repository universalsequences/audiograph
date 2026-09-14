#ifndef GRAPH_TYPES_H
#define GRAPH_TYPES_H

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ===================== Constants =====================
#define MAX_IO 32 // Increased to support larger mixer nodes

// ===================== Helpers =====================
static inline void *alloc_aligned(size_t alignment, size_t size) {
  void *p = NULL;
  if (posix_memalign(&p, alignment, size) != 0)
    return NULL;
  return p;
}

static inline uint64_t nsec_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// ===================== Kernel ABI =====================
typedef void (*KernelFn)(float *const *in, float *const *out, int nframes,
                         void *state);
typedef void (*InitFn)(void *state, int sampleRate, int maxBlock);
typedef void (*ResetFn)(void *state);
typedef void (*MigrateFn)(void *newState, const void *oldState);

typedef struct {
  KernelFn process;
  InitFn init;       // optional
  ResetFn reset;     // optional
  MigrateFn migrate; // optional: copy persistent state on graph swap
} NodeVTable;

// ===================== Parameter Mailbox =====================
typedef enum { PARAM_SET_GAIN = 1 } ParamKind;

typedef struct {
  uint64_t idx;        // target parameter index
  uint64_t logical_id; // target node
  float fvalue;        // e.g., new gain
} ParamMsg;

#define PARAM_RING_CAP 256

typedef struct ParamRing {
  ParamMsg buf[PARAM_RING_CAP];
  _Atomic uint32_t head; // producer writes
  _Atomic uint32_t tail; // consumer reads
} ParamRing;

// ===================== MPMC Work Queue =====================
// Vyukov-style MPMC bounded queue with per-cell sequence numbers
// Fixes the race condition in rb_pop_sc where multiple consumers
// could read the same job or lose jobs entirely.

typedef struct {
  _Atomic uint64_t sequence; // Per-cell sequence number for ABA protection
  int32_t value;             // Node ID to process
  char padding[64 - sizeof(_Atomic uint64_t) -
               sizeof(int32_t)]; // Cache line align
} __attribute__((aligned(64))) MPMCCell;

typedef struct {
  MPMCCell *buffer; // Ring buffer of cells
  uint32_t mask;    // Capacity - 1 (must be power of 2)
  char padding1[64 - sizeof(MPMCCell *) - sizeof(uint32_t)];
  _Atomic uint64_t head; // Producer cursor
  char padding2[64 - sizeof(_Atomic uint64_t)];
  _Atomic uint64_t tail; // Consumer cursor
  char padding3[64 - sizeof(_Atomic uint64_t)];
} __attribute__((aligned(64))) MPMCQueue;

// MPMC queue operations
static inline MPMCQueue *mpmc_create(uint32_t capacity) {
  // Capacity must be power of 2
  if ((capacity & (capacity - 1)) != 0)
    return NULL;

  MPMCQueue *q = (MPMCQueue *)alloc_aligned(64, sizeof(MPMCQueue));
  if (!q)
    return NULL;

  q->buffer = (MPMCCell *)alloc_aligned(64, capacity * sizeof(MPMCCell));
  if (!q->buffer) {
    free(q);
    return NULL;
  }

  q->mask = capacity - 1;
  atomic_store_explicit(&q->head, 0, memory_order_relaxed);
  atomic_store_explicit(&q->tail, 0, memory_order_relaxed);

  // Initialize sequence numbers
  for (uint32_t i = 0; i < capacity; ++i) {
    atomic_store_explicit(&q->buffer[i].sequence, i, memory_order_relaxed);
  }

  return q;
}

static inline void mpmc_destroy(MPMCQueue *q) {
  if (q) {
    free(q->buffer);
    free(q);
  }
}

static inline bool mpmc_push(MPMCQueue *q, int32_t value) {
  MPMCCell *cell;
  uint64_t pos = atomic_load_explicit(&q->head, memory_order_relaxed);

  for (;;) {
    cell = &q->buffer[pos & q->mask];
    uint64_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
    intptr_t dif = (intptr_t)seq - (intptr_t)pos;

    if (dif == 0) {
      // Cell is available for writing
      if (atomic_compare_exchange_weak_explicit(&q->head, &pos, pos + 1,
                                                memory_order_relaxed,
                                                memory_order_relaxed))
        break;
    } else if (dif < 0) {
      // Queue is full
      return false;
    } else {
      // Another producer got ahead, reload position
      pos = atomic_load_explicit(&q->head, memory_order_relaxed);
    }
  }

  cell->value = value;
  atomic_store_explicit(&cell->sequence, pos + 1, memory_order_release);
  return true;
}

static inline bool mpmc_pop(MPMCQueue *q, int32_t *value) {
  MPMCCell *cell;
  uint64_t pos = atomic_load_explicit(&q->tail, memory_order_relaxed);

  for (;;) {
    cell = &q->buffer[pos & q->mask];
    uint64_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
    intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);

    if (dif == 0) {
      // Cell is ready for reading
      if (atomic_compare_exchange_weak_explicit(&q->tail, &pos, pos + 1,
                                                memory_order_relaxed,
                                                memory_order_relaxed))
        break;
    } else if (dif < 0) {
      // Queue is empty
      return false;
    } else {
      // Another consumer got ahead, reload position
      pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
    }
  }

  *value = cell->value;
  atomic_store_explicit(&cell->sequence, pos + q->mask + 1,
                        memory_order_release);
  return true;
}

// Parameter ring operations
static inline bool params_push(ParamRing *r, ParamMsg m) {
  uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
  uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
  if ((h - t) >= PARAM_RING_CAP)
    return false; // full
  r->buf[h % PARAM_RING_CAP] = m;
  atomic_store_explicit(&r->head, h + 1, memory_order_release);
  return true;
}

static inline bool params_pop(ParamRing *r, ParamMsg *out) {
  uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
  uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
  if (t == h)
    return false; // empty
  *out = r->buf[t % PARAM_RING_CAP];
  atomic_store_explicit(&r->tail, t + 1, memory_order_release);
  return true;
}

typedef enum {
  GE_ADD_NODE,
  GE_REMOVE_NODE,
  GE_CONNECT,
  GE_DISCONNECT,
} GraphEditOp;

typedef struct {
  GraphEditOp op;
  union {
    struct {
      NodeVTable vt;
      void *state;
      uint64_t logical_id;
      char *name;
      int nInputs;
      int nOutputs;
    } add_node;
    struct {
      int node_id;
    } remove_node;
    struct {
      int src_id, src_port, dst_id, dst_port;
    } connect;
    struct {
      int src_id, src_port, dst_id, dst_port;
    } disconnect;
  } u;
} GraphEditCmd;

typedef struct GraphEditQueue {
  GraphEditCmd *buf;
  uint32_t cap;  // power-of-two
  uint32_t mask; // cap - 1

  _Atomic uint32_t head; // producer writes
  _Atomic uint32_t tail; // consumer reads
} GraphEditQueue;

#endif // GRAPH_TYPES_H
