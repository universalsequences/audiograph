#ifndef GRAPH_EDIT_H_
#define GRAPH_EDIT_H_

#include "graph_engine.h"

static inline bool geq_init(GraphEditQueue *q, uint32_t capacity_pow2) {
  if (!q) {
    printf("GEQ FAILEDDDDDDDDDDDDDDDDDDDDDDDDD B/C !q\n");
    return false;
  }

  q->cap = capacity_pow2;
  q->mask = capacity_pow2 - 1;

  printf("GEQ CAP INIT CAP=%d\n", q->cap);

  q->buf = (GraphEditCmd *)calloc(q->cap, sizeof(GraphEditCmd));
  if (!q->buf) {
    printf("GEQ INIT FAILED BETCAUSE NO BUF!\n");
    return false;
  }

  atomic_init(&q->head, 0);
  atomic_init(&q->tail, 0);

  return true;
}

static inline void geq_deinit(GraphEditQueue *q) {
  if (!q) {
    return;
  }
  free(q->buf);
  q->buf = NULL;
}

static inline bool geq_push(GraphEditQueue *r, const GraphEditCmd *cmd) {
  // get sequence numbers
  uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
  uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
  uint32_t next = head + 1;

  printf("NOTICE: GEQ_PUSH CALLED WITH head=%d tail=%d next=%d cap=%d\n", head,
         tail, next, r->cap);

  if ((next & r->mask) == (tail & r->mask)) {
    printf("geq was full head=%d tail=%d next=%d cap=%d\n", head, tail, next,
           r->cap);
    // we're full so ignoring
    return false;
  }

  r->buf[head & r->mask] = *cmd;

  // publish the "next" (not masked so we can keep track of the unwrapped value
  // and do proper head - tail calculations)
  // Note: this avoid ambiguity when next % cap == tail % cap
  atomic_store_explicit(&r->head, next, memory_order_relaxed);
  return true;
}

static inline bool geq_pop(GraphEditQueue *r, GraphEditCmd *cmd) {
  uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
  uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);

  if ((tail & r->mask) == (head & r->mask)) {
    // empty
    return false;
  }

  *cmd = r->buf[tail & r->mask];

  atomic_store_explicit(&r->tail, tail + 1, memory_order_relaxed);
  return true;
}

bool apply_graph_edits(GraphEditQueue *r, LiveGraph *lg);

#endif // GRAPH_EDIT_H_
