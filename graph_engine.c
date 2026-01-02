#include "graph_engine.h"
#include "graph_edit.h"
#include "graph_nodes.h"
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// On Apple platforms, enable QoS hints for worker threads to reduce jitter.
#ifdef __APPLE__
#if __has_include(<pthread/qos.h>)
#include <pthread/qos.h>
#endif
#if __has_include(<os/workgroup.h>)
#include <os/workgroup.h>
#define HAVE_OS_WORKGROUP 1
#endif
#if __has_include(<mach/mach.h>)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#define HAVE_MACH_RT 1
#endif
#endif

// Include ReadyQ implementation
extern ReadyQ *rq_create(int capacity);
extern void rq_destroy(ReadyQ *q);
extern bool rq_push(ReadyQ *q, int32_t nid);
extern bool rq_try_pop(ReadyQ *q, int32_t *out);
extern bool rq_wait_nonempty(ReadyQ *q, int timeout_us);
extern void rq_reset(ReadyQ *q);
extern void rq_push_or_spin(ReadyQ *q, int32_t nid);
extern void rq_push_batch(ReadyQ *q, const int32_t *nids, int count);

// ===================== Forward Declarations =====================

void bind_and_run_live(LiveGraph *lg, int nid, int nframes);
static bool ensure_port_arrays(RTNode *n);
static void init_pending_and_seed(LiveGraph *lg);
static int alloc_edge(LiveGraph *lg);
void process_live_block(LiveGraph *lg, int nframes);
static inline void execute_and_fanout(LiveGraph *lg, int32_t nid, int nframes);
static void wait_for_block_start_or_shutdown(void);

// ===================== Global Engine Instance =====================

Engine g_engine; // single global for demo

// ===================== SUM Node Input Count Tracking =====================

// Thread-local storage for current node being processed
static __thread RTNode *g_current_processing_node = NULL;

// Thread-local storage for per-node IO binding
static __thread float **tls_node_inputs = NULL;
static __thread float **tls_node_outputs = NULL;
static __thread int tls_io_capacity = 0;

// Thread-local scratch buffers for disconnected outputs (grow-on-demand)
static __thread float **tls_out_scratch = NULL;
static __thread size_t *tls_out_scratch_sizes = NULL;
static __thread int tls_scratch_ports = 0;

static bool ensure_tls_io_capacity(int needed_inputs, int needed_outputs) {
  int required =
      needed_inputs > needed_outputs ? needed_inputs : needed_outputs;
  if (required <= tls_io_capacity)
    return true;
  if (required <= 0)
    return true;

  int new_cap = tls_io_capacity ? tls_io_capacity : MAX_IO;
  while (new_cap < required) {
    new_cap *= 2;
  }
  if (new_cap <= 0)
    new_cap = required;

  float **new_inputs = malloc((size_t)new_cap * sizeof(float *));
  float **new_outputs = malloc((size_t)new_cap * sizeof(float *));
  if (!new_inputs || !new_outputs) {
    if (new_inputs)
      free(new_inputs);
    if (new_outputs)
      free(new_outputs);
    return false;
  }

  if (tls_node_inputs) {
    memcpy(new_inputs, tls_node_inputs,
           (size_t)tls_io_capacity * sizeof(float *));
    free(tls_node_inputs);
  }
  if (tls_node_outputs) {
    memcpy(new_outputs, tls_node_outputs,
           (size_t)tls_io_capacity * sizeof(float *));
    free(tls_node_outputs);
  }
  for (int i = tls_io_capacity; i < new_cap; ++i) {
    new_inputs[i] = NULL;
    new_outputs[i] = NULL;
  }

  tls_node_inputs = new_inputs;
  tls_node_outputs = new_outputs;
  tls_io_capacity = new_cap;
  return true;
}

static inline float *get_tls_out_scratch(LiveGraph *lg, int port, int nframes) {
  if (port < 0)
    return lg ? lg->scratch_null : NULL;

  if (port >= tls_scratch_ports) {
    int new_ports = tls_scratch_ports ? tls_scratch_ports : MAX_IO;
    while (new_ports <= port)
      new_ports *= 2;

    float **new_buffers = calloc((size_t)new_ports, sizeof(float *));
    size_t *new_sizes = calloc((size_t)new_ports, sizeof(size_t));
    if (!new_buffers || !new_sizes) {
      if (new_buffers)
        free(new_buffers);
      if (new_sizes)
        free(new_sizes);
      return lg ? lg->scratch_null : NULL;
    }

    if (tls_out_scratch) {
      memcpy(new_buffers, tls_out_scratch,
             (size_t)tls_scratch_ports * sizeof(float *));
      free(tls_out_scratch);
    }
    if (tls_out_scratch_sizes) {
      memcpy(new_sizes, tls_out_scratch_sizes,
             (size_t)tls_scratch_ports * sizeof(size_t));
      free(tls_out_scratch_sizes);
    }

    tls_out_scratch = new_buffers;
    tls_out_scratch_sizes = new_sizes;
    tls_scratch_ports = new_ports;
  }

  if ((size_t)nframes > tls_out_scratch_sizes[port]) {
    float *new_buf = aligned_alloc(64, (size_t)nframes * sizeof(float));
    if (!new_buf) {
      new_buf = malloc((size_t)nframes * sizeof(float));
      if (!new_buf)
        return lg ? lg->scratch_null : NULL;
    }
    if (tls_out_scratch[port]) {
      free(tls_out_scratch[port]);
    }
    memset(new_buf, 0, (size_t)nframes * sizeof(float));
    tls_out_scratch[port] = new_buf;
    tls_out_scratch_sizes[port] = (size_t)nframes;
  }

  return tls_out_scratch[port];
}

int ap_current_node_ninputs(void) {
  if (g_current_processing_node) {
    return g_current_processing_node->nInputs;
  }
  return 0; // fallback
}

void initialize_engine(int block_Size, int sample_rate) {
  g_engine.blockSize = block_Size;
  g_engine.sampleRate = sample_rate;
  atomic_store_explicit(&g_engine.oswg, NULL, memory_order_relaxed);
  atomic_store_explicit(&g_engine.oswg_join_pending, 0, memory_order_relaxed);
  atomic_store_explicit(&g_engine.oswg_join_remaining, 0, memory_order_relaxed);
  atomic_store_explicit(&g_engine.oswg_version, 0, memory_order_relaxed);
  atomic_store_explicit(&g_engine.rt_time_constraint, 0, memory_order_relaxed);
}

// ===================== Graph Management =====================

// ===================== Parameter Application =====================

void apply_params(LiveGraph *g) {
  if (!g || !g->params)
    return;
  ParamMsg m;
  while (params_pop(g->params, &m)) {
    // O(1) direct lookup: logical_id is used as the array index in apply_add_node
    int node_id = (int)m.logical_id;
    if (node_id >= 0 && node_id < g->node_count) {
      RTNode *node = &g->nodes[node_id];
      // Verify logical_id matches (safety check for deleted/reused slots)
      if (node->state && node->logical_id == m.logical_id) {
        float *memory = (float *)node->state;
        memory[m.idx] = m.fvalue;
      }
    }
  }
}

// ===================== Block Processing =====================

// Legacy bind_and_run function removed - using port-based bind_and_run_live
// only

static void wait_for_block_start_or_shutdown(void) {
  pthread_mutex_lock(&g_engine.sess_mtx);
  for (;;) {
    if (!atomic_load_explicit(&g_engine.runFlag, memory_order_acquire))
      break;
    // Also wake if workgroup join is pending
    if (atomic_load_explicit(&g_engine.oswg_join_pending, memory_order_acquire))
      break;
    LiveGraph *lg =
        atomic_load_explicit(&g_engine.workSession, memory_order_acquire);
    if (lg && atomic_load_explicit(&lg->jobsInFlight, memory_order_acquire) > 0)
      break;
    pthread_cond_wait(&g_engine.sess_cv, &g_engine.sess_mtx);
  }
  pthread_mutex_unlock(&g_engine.sess_mtx);
}

static void *worker_main(void *arg) {
  (void)arg;
  // Elevate worker thread QoS on Apple platforms for better scheduling.
#ifdef __APPLE__
#ifdef QOS_CLASS_USER_INTERACTIVE
  (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
#endif

#ifdef HAVE_MACH_RT
  // Optionally promote to Mach time-constraint scheduling
  if (atomic_load_explicit(&g_engine.rt_time_constraint,
                           memory_order_acquire)) {
    // Compute period from engine config
    double sr =
        (g_engine.sampleRate > 0) ? (double)g_engine.sampleRate : 48000.0;
    double bs = (g_engine.blockSize > 0) ? (double)g_engine.blockSize : 512.0;
    double period_ns_d = (bs / sr) * 1e9; // block duration in ns
    uint64_t period_ns = (uint64_t)(period_ns_d + 0.5);
    // Budget ~75% of period, constraint = period
    uint64_t comp_ns = (period_ns * 3) / 4;
    uint64_t cons_ns = period_ns;

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t period_abs = (period_ns * tb.denom) / tb.numer;
    uint64_t comp_abs = (comp_ns * tb.denom) / tb.numer;
    uint64_t cons_abs = (cons_ns * tb.denom) / tb.numer;

    thread_time_constraint_policy_data_t pol;
    pol.period = (uint32_t)period_abs;
    pol.computation = (uint32_t)comp_abs;
    pol.constraint = (uint32_t)cons_abs;
    pol.preemptible = TRUE;

    kern_return_t kr = thread_policy_set(
        mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
      fprintf(stderr,
              "[audiograph] WARN: thread_policy_set RT failed (kr=%d)\n", kr);
    } else {
      fprintf(stderr,
              "[audiograph] worker %p set Mach RT TC (period=%.2f ms)\n",
              (void *)pthread_self(), period_ns_d / 1e6);
    }
  }
#endif

#ifdef HAVE_OS_WORKGROUP
  os_workgroup_t oswg = NULL;
  os_workgroup_join_token_s oswg_token;  // Stack-allocated token struct (not pointer)
  memset(&oswg_token, 0, sizeof(oswg_token));
  bool oswg_joined = false;
  int oswg_local_version = 0; // Track which version we've joined
#endif
  for (;;) {
    if (!atomic_load_explicit(&g_engine.runFlag, memory_order_acquire))
      break;

    // Park until a block is published
    wait_for_block_start_or_shutdown();
    if (!atomic_load_explicit(&g_engine.runFlag, memory_order_acquire))
      break;

    // Handle OS workgroup joining/re-joining when version changes
    // This allows workers to switch workgroups without being recreated
#ifdef HAVE_OS_WORKGROUP
    int global_version = atomic_load_explicit(&g_engine.oswg_version, memory_order_acquire);
    if (global_version != oswg_local_version) {
      // Version changed - need to leave old workgroup and join new one
      // IMPORTANT: We must leave using our saved oswg pointer and token,
      // not the global one (which may have changed or been freed)
      if (oswg_joined) {
        // We have a valid join - leave using our saved references
        os_workgroup_leave(oswg, &oswg_token);
        fprintf(stderr, "[audiograph] worker %p left os_workgroup %p (version %d -> %d)\n",
                (void *)pthread_self(), (void *)oswg, oswg_local_version, global_version);
        oswg_joined = false;
        oswg = NULL;
        memset(&oswg_token, 0, sizeof(oswg_token));
      }

      // Update local version before attempting join
      oswg_local_version = global_version;

      void *w = atomic_load_explicit(&g_engine.oswg, memory_order_acquire);
      if (w) {
        oswg = (os_workgroup_t)w;
        int ok = os_workgroup_join(oswg, &oswg_token);
        oswg_joined = (ok == 0);
        if (oswg_joined) {
          fprintf(stderr, "[audiograph] worker %p joined os_workgroup %p (version %d)\n",
                  (void *)pthread_self(), (void *)oswg, global_version);
        } else {
          fprintf(stderr, "[audiograph] worker %p FAILED to join os_workgroup %p (err=%d)\n",
                  (void *)pthread_self(), (void *)oswg, ok);
          oswg = NULL;  // Don't keep stale pointer on failure
        }
      }
      // Decrement remaining counter; last worker clears the pending flag
      int remaining = atomic_fetch_sub_explicit(&g_engine.oswg_join_remaining, 1,
                                                memory_order_acq_rel) - 1;
      if (remaining == 0) {
        atomic_store_explicit(&g_engine.oswg_join_pending, 0, memory_order_release);
      }
    }
#endif

    LiveGraph *lg =
        atomic_load_explicit(&g_engine.workSession, memory_order_acquire);
    if (!lg)
      continue; // spurious wake or no work - but workgroup joining is done

    // Hot loop: run until this block is complete
    for (;;) {
      // If the session ended or graph pointer changed, exit the hot loop.
      LiveGraph *cur =
          atomic_load_explicit(&g_engine.workSession, memory_order_acquire);
      if (cur != lg)
        break;
      if (atomic_load_explicit(&lg->jobsInFlight, memory_order_acquire) == 0)
        break;

      int32_t nid;

      // Tiny spin to catch bursts without kernel call, then short timed wait
      bool got = false;
      for (int s = 0; s < 64; s++) {
        if ((got = rq_try_pop(lg->readyQueue, &nid)))
          break;
        cpu_relax(); // brief pause
      }
      if (!got) {
        // Queue appears empty; wait a very short time for a wake signal
        (void)rq_wait_nonempty(lg->readyQueue, /*timeout_us=*/10);
        continue;
      }

      // Validate job ID to avoid crashes if queue is corrupted under load
      if (nid < 0 || nid >= lg->node_count) {
        fprintf(stderr,
                "[audiograph] WARN: invalid job id %d (node_count=%d)\n", nid,
                lg->node_count);
        continue;
      }

      int nf =
          atomic_load_explicit(&g_engine.sessionFrames, memory_order_acquire);
      if (nf <= 0 || nf > lg->block_size) {
        nf = lg->block_size; // Clamp to graph's internal block size for safety
      }
      execute_and_fanout(lg, nid, nf);
    }

    // Loop back: will go to sleep on sess_cv until next block
  }

  // Thread exiting: leave workgroup if still joined
#ifdef HAVE_OS_WORKGROUP
  if (oswg_joined && oswg) {
    os_workgroup_leave(oswg, &oswg_token);
    oswg_joined = false;
  }
#endif

  return NULL;
}

// ===================== Worker Pool Management =====================

void engine_start_workers(int workers) {
  g_engine.workerCount = workers;
  g_engine.threads = (pthread_t *)calloc(workers, sizeof(pthread_t));

  // Initialize mutex and condition variable for block-start wake
  pthread_mutex_init(&g_engine.sess_mtx, NULL);
  pthread_cond_init(&g_engine.sess_cv, NULL);

  atomic_store(&g_engine.runFlag, 1);
  for (int i = 0; i < workers; i++) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Hint a high QoS class on Apple platforms; no-ops elsewhere.
#ifdef __APPLE__
#ifdef QOS_CLASS_USER_INTERACTIVE
    (void)pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
#endif
#endif
    pthread_create(&g_engine.threads[i], &attr, worker_main, NULL);
    pthread_attr_destroy(&attr);
  }
}

void engine_set_os_workgroup(void *oswg_ptr) {
#ifdef HAVE_OS_WORKGROUP
  // Store opaque pointer; Swift side retains it.
  atomic_store_explicit(&g_engine.oswg, oswg_ptr, memory_order_release);

  // Increment version to signal workers to re-join
  int new_version = atomic_fetch_add_explicit(&g_engine.oswg_version, 1,
                                              memory_order_acq_rel) + 1;

  // Set counter to number of workers, then set flag and broadcast
  // This ensures all workers see the flag before it's cleared
  atomic_store_explicit(&g_engine.oswg_join_remaining, g_engine.workerCount,
                        memory_order_release);
  atomic_store_explicit(&g_engine.oswg_join_pending, 1, memory_order_release);
  pthread_mutex_lock(&g_engine.sess_mtx);
  pthread_cond_broadcast(&g_engine.sess_cv);
  pthread_mutex_unlock(&g_engine.sess_mtx);

  fprintf(stderr,
          "[audiograph] set os_workgroup=%p (version %d, notifying %d existing workers)\n",
          oswg_ptr, new_version, g_engine.workerCount);
#else
  (void)oswg_ptr;
  fprintf(
      stderr,
      "[audiograph] os_workgroup unsupported at compile time; ignoring set\n");
#endif
}

void engine_clear_os_workgroup(void) {
#ifdef HAVE_OS_WORKGROUP
  // First, signal workers to leave by setting NULL and incrementing version
  atomic_store_explicit(&g_engine.oswg, NULL, memory_order_release);
  int new_version = atomic_fetch_add_explicit(&g_engine.oswg_version, 1,
                                              memory_order_acq_rel) + 1;

  atomic_store_explicit(&g_engine.oswg_join_remaining, g_engine.workerCount,
                        memory_order_release);
  atomic_store_explicit(&g_engine.oswg_join_pending, 1, memory_order_release);
  pthread_mutex_lock(&g_engine.sess_mtx);
  pthread_cond_broadcast(&g_engine.sess_cv);
  pthread_mutex_unlock(&g_engine.sess_mtx);

  fprintf(stderr,
          "[audiograph] clearing os_workgroup (version %d, waiting for %d workers to leave)\n",
          new_version, g_engine.workerCount);

  // IMPORTANT: Wait for all workers to leave before returning
  // This ensures Swift can safely release the old workgroup
  int timeout_ms = 1000;  // 1 second timeout
  int waited_ms = 0;
  while (atomic_load_explicit(&g_engine.oswg_join_pending, memory_order_acquire) != 0) {
    usleep(1000);  // 1ms
    waited_ms++;
    if (waited_ms >= timeout_ms) {
      int remaining = atomic_load_explicit(&g_engine.oswg_join_remaining, memory_order_acquire);
      fprintf(stderr,
              "[audiograph] WARNING: timeout waiting for workers to leave workgroup (%d remaining)\n",
              remaining);
      break;
    }
  }

  fprintf(stderr, "[audiograph] os_workgroup cleared (workers left in %d ms)\n", waited_ms);
#else
  fprintf(stderr, "[audiograph] os_workgroup unsupported; clear ignored\n");
#endif
}

void engine_enable_rt_time_constraint(int enable) {
  atomic_store_explicit(&g_engine.rt_time_constraint, enable ? 1 : 0,
                        memory_order_release);
}

void engine_stop_workers(void) {
  atomic_store(&g_engine.runFlag, 0);

  // Wake sleepers on both wait sites
  pthread_mutex_lock(&g_engine.sess_mtx);
  pthread_cond_broadcast(&g_engine.sess_cv);
  pthread_mutex_unlock(&g_engine.sess_mtx);

  // Also wake any workers blocked in rq_wait_nonempty during a block
  // We'll iterate through all potential live graphs, but since we're shutting
  // down, we can just wait for threads to exit naturally

  for (int i = 0; i < g_engine.workerCount; i++) {
    pthread_join(g_engine.threads[i], NULL);
  }

  // Clean up synchronization primitives
  pthread_mutex_destroy(&g_engine.sess_mtx);
  pthread_cond_destroy(&g_engine.sess_cv);

  free(g_engine.threads);
  g_engine.threads = NULL;
  g_engine.workerCount = 0;
}

// ===================== Live Graph Operations =====================

// Rebuild IO cache for a single node (called lazily when cache is invalid)
static void rebuild_node_io_cache(LiveGraph *lg, RTNode *node, int nframes) {
  (void)nframes;  // Currently unused but might be needed later

  // Reallocate cached pointer arrays if size changed
  // This handles cases where SUM nodes grow their input count
  if (node->nInputs > 0) {
    if (node->cached_inPtrs) {
      free(node->cached_inPtrs);
    }
    node->cached_inPtrs = malloc(node->nInputs * sizeof(float *));
  }
  if (node->nOutputs > 0) {
    if (node->cached_outPtrs) {
      free(node->cached_outPtrs);
    }
    node->cached_outPtrs = malloc(node->nOutputs * sizeof(float *));
  }

  // Resolve input pointers
  if (node->cached_inPtrs) {
    for (int i = 0; i < node->nInputs; i++) {
      int eid = node->inEdgeId ? node->inEdgeId[i] : -1;
      if (eid >= 0 && eid < lg->edge_capacity && lg->edges[eid].buf) {
        node->cached_inPtrs[i] = lg->edges[eid].buf;
      } else {
        node->cached_inPtrs[i] = lg->silence_buf;
      }
    }
  }

  // Resolve output pointers
  if (node->cached_outPtrs) {
    for (int i = 0; i < node->nOutputs; i++) {
      int eid = node->outEdgeId ? node->outEdgeId[i] : -1;
      if (eid >= 0 && eid < lg->edge_capacity && lg->edges[eid].buf) {
        node->cached_outPtrs[i] = lg->edges[eid].buf;
      } else {
        node->cached_outPtrs[i] = lg->scratch_null;
      }
    }
  }

  node->io_cache_valid = true;
}

void bind_and_run_live(LiveGraph *lg, int nid, int nframes) {
  RTNode *node = &lg->nodes[nid];

  // treat deleted nodes as: no process fn AND no ports
  if (node->vtable.process == NULL && node->nInputs == 0 && node->nOutputs == 0)
    return;
  if (lg->is_orphaned[nid]) // Node is orphaned
    return;
  if (node->nInputs < 0 || node->nOutputs < 0) // Invalid port counts
    return;

  // Set thread-local context for SUM nodes to access input count
  g_current_processing_node = node;

  // === OPTIMIZATION: Use pre-cached IO pointers ===
  // Only rebuild if cache is invalid (topology changed)
  if (!node->io_cache_valid) {
    rebuild_node_io_cache(lg, node, nframes);
  }

  // Use cached pointers directly - no per-block loops!
  float **inPtrs = node->cached_inPtrs;
  float **outPtrs = node->cached_outPtrs;

  // Fallback to silence/scratch if no cached pointers (shouldn't happen)
  if (!inPtrs && node->nInputs > 0) {
    inPtrs = &lg->silence_buf;  // Single pointer fallback
  }
  if (!outPtrs && node->nOutputs > 0) {
    outPtrs = &lg->scratch_null;
  }

  if (node->vtable.process) {
    node->vtable.process((float *const *)inPtrs, (float *const *)outPtrs,
                         nframes, node->state, lg->buffers);
  }

  // Clear thread-local context
  g_current_processing_node = NULL;
}


static inline void execute_and_fanout(LiveGraph *lg, int32_t nid, int nframes) {
  if (nid < 0 || nid >= lg->node_count) {
    fprintf(stderr,
            "[audiograph] WARN: execute_and_fanout skipping invalid nid=%d "
            "(count=%d)\n",
            nid, lg->node_count);
    return;
  }
  bind_and_run_live(lg, nid, nframes); // uses silence/scratch for missing ports

  RTNode *node = &lg->nodes[nid];

  // OPTIMIZATION: Check if we have any successors before the loop
  // This avoids cache misses on is_orphaned for leaf nodes
  if (node->succCount > 0) {
    // Notify successors (node-level)
    // Use release semantics to ensure our output buffer writes are visible
    for (int i = 0; i < node->succCount; i++) {
      int succ = node->succ[i];
      if (succ < 0 || succ >= lg->node_count) {
        continue;
      }
      if (lg->is_orphaned[succ]) {
        continue;
      }
      // Use release on decrement to ensure buffer writes are visible to successor
      if (atomic_fetch_sub_explicit(&lg->pending[succ], 1, memory_order_release) == 1) {
        rq_push_or_spin(lg->readyQueue, succ);
      }
    }
  }

  // Relaxed is fine here - just a counter
  atomic_fetch_sub_explicit(&lg->jobsInFlight, 1, memory_order_relaxed);
}

// Check if a node has any connected outputs (for scheduling)
static inline bool node_has_any_output_connected(LiveGraph *lg, int node_id) {
  RTNode *node = &lg->nodes[node_id];
  if (!node->outEdgeId)
    return false;

  for (int i = 0; i < node->nOutputs; i++) {
    if (node->outEdgeId[i] >= 0)
      return true;
  }
  return false;
}

// ===================== OPTIMIZATION: Scheduling Cache =====================
// Instead of O(n) scans every block, we cache source nodes and job counts.
// The cache is rebuilt only when topology changes (scheduling_dirty flag).

static void rebuild_scheduling_cache(LiveGraph *lg) {
  int totalJobs = 0;
  int sourceCount = 0;

  // Count sources first to check capacity
  for (int i = 0; i < lg->node_count; i++) {
    bool deleted = (lg->nodes[i].vtable.process == NULL &&
                    lg->nodes[i].nInputs == 0 && lg->nodes[i].nOutputs == 0);
    if (deleted || lg->is_orphaned[i])
      continue;

    bool hasOut = node_has_any_output_connected(lg, i);
    bool isSink = !hasOut && lg->indegree[i] > 0;

    if (hasOut || isSink) {
      totalJobs++;
      if (lg->indegree[i] == 0 && hasOut) {
        sourceCount++;
      }
    }
  }

  // Grow source_nodes array if needed
  if (sourceCount > lg->source_capacity) {
    int new_cap = lg->source_capacity;
    while (new_cap < sourceCount)
      new_cap *= 2;
    int32_t *new_sources = realloc(lg->source_nodes, new_cap * sizeof(int32_t));
    if (new_sources) {
      lg->source_nodes = new_sources;
      lg->source_capacity = new_cap;
    }
  }

  // Build source list
  lg->source_count = 0;
  for (int i = 0; i < lg->node_count; i++) {
    bool deleted = (lg->nodes[i].vtable.process == NULL &&
                    lg->nodes[i].nInputs == 0 && lg->nodes[i].nOutputs == 0);
    if (deleted || lg->is_orphaned[i])
      continue;

    if (lg->indegree[i] == 0 && node_has_any_output_connected(lg, i)) {
      if (lg->source_count < lg->source_capacity) {
        lg->source_nodes[lg->source_count++] = i;
      }
    }
  }

  // Detect cycles at topology-change time (not every block!)
  lg->has_cycle = (totalJobs > 0 && lg->source_count == 0);

  lg->cached_total_jobs = totalJobs;
  lg->scheduling_dirty = false;
}

static void init_pending_and_seed(LiveGraph *lg) {
  // Rebuild cache if topology changed
  if (lg->scheduling_dirty) {
    rebuild_scheduling_cache(lg);
  }

  // CRITICAL FIX: Properly reset/drain the ready queue to prevent stale node
  // IDs. Only drain if there might be stale items.
  int32_t dummy;
  while (rq_try_pop(lg->readyQueue, &dummy)) {
    // Discard any stale items
  }

  // Reset pending counts to indegree for all active nodes
  // This is O(n) but uses relaxed stores which are fast
  // Workers will use atomic decrements on these values
  for (int i = 0; i < lg->node_count; i++) {
    bool deleted = (lg->nodes[i].vtable.process == NULL &&
                    lg->nodes[i].nInputs == 0 && lg->nodes[i].nOutputs == 0);
    if (deleted || lg->is_orphaned[i]) {
      atomic_store_explicit(&lg->pending[i], -1, memory_order_relaxed);
    } else {
      atomic_store_explicit(&lg->pending[i], lg->indegree[i], memory_order_relaxed);
    }
  }

  // Memory barrier to ensure all pending stores are visible before workers start
  atomic_thread_fence(memory_order_release);

  // Seed ready queue from cached source list - O(sources) instead of O(n)
  // Use batch push to reduce semaphore signals from O(sources) to O(1)
  rq_push_batch(lg->readyQueue, lg->source_nodes, lg->source_count);

  atomic_store_explicit(&lg->jobsInFlight, lg->cached_total_jobs,
                        memory_order_release);
}

bool detect_cycle(LiveGraph *lg) {
  // Use cached result if available
  if (!lg->scheduling_dirty) {
    return lg->has_cycle;
  }
  // Fallback to full computation (shouldn't happen in hot path)
  int reachable = 0, zero_in = 0;
  for (int i = 0; i < lg->node_count; i++) {
    if (atomic_load_explicit(&lg->pending[i], memory_order_relaxed) < 0)
      continue; // orphan/deleted
    reachable++;
    if (lg->indegree[i] == 0 && node_has_any_output_connected(lg, i))
      zero_in++;
  }
  return (reachable > 0 && zero_in == 0);
}

// Call at the end of process_live_block (after all work done)
static void drain_retire_list(LiveGraph *lg) {
  for (int i = 0; i < lg->retire_count; i++) {
    lg->retire_list[i].deleter(lg->retire_list[i].ptr);
  }
  lg->retire_count = 0;
}

static void update_watched_node_states(LiveGraph *lg);

void process_live_block(LiveGraph *lg, int nframes) {
  // Initialize pending counts and seed ready queue
  init_pending_and_seed(lg);

  // Check for cycles that would cause silent deadlocks
  if (detect_cycle(lg)) {
    // Clear output buffer to silence
    if (lg->dac_node_id >= 0 && lg->nodes[lg->dac_node_id].inEdgeId) {
      int master_edge_id = lg->nodes[lg->dac_node_id].inEdgeId[0];
      if (master_edge_id >= 0 && master_edge_id < lg->edge_capacity &&
          lg->edges[master_edge_id].buf != NULL) {
        memset(lg->edges[master_edge_id].buf, 0, nframes * sizeof(float));
      }
    }
    update_watched_node_states(lg);
    return;
  }

  // check if no work to be done
  if (atomic_load_explicit(&lg->jobsInFlight, memory_order_acquire) <= 0) {
    update_watched_node_states(lg);
    return;
  }

  if (g_engine.workerCount > 0) {
    // Publish session frames and graph
    atomic_store_explicit(&g_engine.sessionFrames, nframes,
                          memory_order_release);
    atomic_store_explicit(&g_engine.workSession, lg, memory_order_release);

    // wake workers
    pthread_mutex_lock(&g_engine.sess_mtx);
    pthread_cond_broadcast(&g_engine.sess_cv);
    pthread_mutex_unlock(&g_engine.sess_mtx);

    // Audio thread helps do some work
    int32_t nid;
    int empty_spins = 0;
    while (atomic_load_explicit(&lg->jobsInFlight, memory_order_acquire) > 0) {
      if (rq_try_pop(lg->readyQueue, &nid)) {
        execute_and_fanout(lg, nid, nframes);
        empty_spins = 0; // Reset on successful work
      } else {
        // Queue empty but work in flight - workers processing
        // Check again if work completed (avoids unnecessary spins)
        if (atomic_load_explicit(&lg->jobsInFlight, memory_order_acquire) == 0)
          break;
        cpu_relax();
        // After many empty spins, yield to reduce CPU burn
        if (++empty_spins > 64) {
          sched_yield();
          empty_spins = 0;
        }
      }
    }

    // Clear session
    atomic_store_explicit(&g_engine.workSession, NULL, memory_order_release);
  } else {
    // Single-thread fallback
    int32_t nid;
    while (rq_try_pop(lg->readyQueue, &nid)) {
      execute_and_fanout(lg, nid, nframes);
    }
  }

  drain_retire_list(lg);

  // Update watched node states after processing this block (covers both
  // direct process_live_block callers and the process_next_block wrapper).
  update_watched_node_states(lg);
}

int find_live_output(LiveGraph *lg) {
  return lg->dac_node_id; // Simply return the DAC node - no searching needed
}

// ===================== Live Engine Implementation =====================
void process_next_block(LiveGraph *lg, float *output_buffer, int nframes) {
  if (!lg || !output_buffer || nframes <= 0) {
    // Clear output buffer if invalid input
    if (output_buffer && nframes > 0) {
      memset(output_buffer, 0,
             (size_t)nframes * (size_t)lg->num_channels * sizeof(float));
    }
    return;
  }

  apply_graph_edits(lg->graphEditQueue, lg);

  apply_params(lg);

  // Process in slices if callback frames exceed internal block size.
  int remaining = nframes;
  int out_offset = 0; // in frames
  while (remaining > 0) {
    int slice = remaining;
    if (slice > lg->block_size)
      slice = lg->block_size;

    process_live_block(lg, slice);

    // Get the DAC node (final output)
    int output_node = find_live_output(lg);

    if (output_node >= 0 && lg->nodes[output_node].nInputs > 0) {
      RTNode *dac = &lg->nodes[output_node];

      // Copy each channel from DAC inputs to interleaved output buffer
      for (int ch = 0; ch < lg->num_channels; ch++) {
        float *src = NULL;

        // Get the input edge for this channel
        if (ch < dac->nInputs) {
          int edge_id = dac->inEdgeId[ch];
          if (edge_id >= 0 && edge_id < lg->edge_capacity) {
            src = lg->edges[edge_id].buf;
          }
        }

        // Interleave this channel into the output buffer with offset
        float *dst = output_buffer +
                     ((size_t)out_offset * (size_t)lg->num_channels) + ch;
        if (src) {
          for (int i = 0; i < slice; i++) {
            dst[i * lg->num_channels] = src[i];
          }
        } else {
          for (int i = 0; i < slice; i++) {
            dst[i * lg->num_channels] = 0.0f;
          }
        }
      }
    } else {
      // No output node - silence for this slice
      for (int i = 0; i < slice; i++) {
        for (int ch = 0; ch < lg->num_channels; ch++) {
          output_buffer[((size_t)out_offset + i) * (size_t)lg->num_channels +
                        ch] = 0.0f;
        }
      }
    }

    remaining -= slice;
    out_offset += slice;
  }

  // Update watched node states after processing
  update_watched_node_states(lg);
}

static void update_watched_node_states(LiveGraph *lg) {
  if (!lg || lg->watch_list_count == 0) {
    return;
  }

  // Atomically fetch current watchlist
  pthread_mutex_lock(&lg->watch_list_mutex);
  int watch_count = lg->watch_list_count;
  int *watch_nodes = malloc(watch_count * sizeof(int));
  if (!watch_nodes) {
    pthread_mutex_unlock(&lg->watch_list_mutex);
    return;
  }
  memcpy(watch_nodes, lg->watch_list, watch_count * sizeof(int));
  pthread_mutex_unlock(&lg->watch_list_mutex);

  // Update state snapshots for watched nodes
  pthread_rwlock_wrlock(&lg->state_store_lock);

  for (int i = 0; i < watch_count; i++) {
    int node_id = watch_nodes[i];

    // Validate node_id and check if node exists
    if (node_id < 0 || node_id >= lg->node_count) {
      continue;
    }

    RTNode *node = &lg->nodes[node_id];
    if (!node->state || node->state_size == 0) {
      continue; // No state to copy
    }

    // Reuse existing snapshot buffer if size matches; avoid per-block
    // malloc/free
    if (lg->state_snapshots[node_id] &&
        lg->state_sizes[node_id] == node->state_size) {
      memcpy(lg->state_snapshots[node_id], node->state, node->state_size);
    } else {
      // Size changed or no buffer yet; (re)allocate
      if (lg->state_snapshots[node_id]) {
        free(lg->state_snapshots[node_id]);
        lg->state_snapshots[node_id] = NULL;
        lg->state_sizes[node_id] = 0;
      }
      void *snapshot = malloc(node->state_size);
      if (snapshot) {
        memcpy(snapshot, node->state, node->state_size);
        lg->state_snapshots[node_id] = snapshot;
        lg->state_sizes[node_id] = node->state_size;
      }
    }
  }

  pthread_rwlock_unlock(&lg->state_store_lock);
  free(watch_nodes);
}
