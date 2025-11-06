#include "graph_engine.h"
#include "graph_edit.h"
#include "graph_nodes.h"
#include <assert.h>
#include <stdio.h>
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

// Thread-local scratch buffers for disconnected outputs
// This prevents the write-write race that caused audio artifacts
static __thread float *tls_out_scratch[MAX_IO] = {0};
static __thread int tls_scratch_size = 0;

static inline float *get_tls_out_scratch(int port, int nframes) {
  if (tls_scratch_size < nframes) {
    // Reallocate all scratch buffers for this thread
    for (int p = 0; p < MAX_IO; p++) {
      free(tls_out_scratch[p]);
      tls_out_scratch[p] = aligned_alloc(64, nframes * sizeof(float));
      if (!tls_out_scratch[p]) {
        // Fallback to regular malloc if aligned_alloc fails
        tls_out_scratch[p] = malloc(nframes * sizeof(float));
      }
    }
    tls_scratch_size = nframes;
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
  atomic_store_explicit(&g_engine.rt_time_constraint, 0, memory_order_relaxed);
}

// ===================== Graph Management =====================

// ===================== Parameter Application =====================

void apply_params(LiveGraph *g) {
  if (!g || !g->params)
    return;
  ParamMsg m;
  while (params_pop(g->params, &m)) {
    for (int i = 0; i < g->node_count; i++) {
      if (g->nodes[i].logical_id == m.logical_id) {
        if (g->nodes[i].state) { // Only apply if node has memory
          float *memory = (float *)g->nodes[i].state;
          memory[m.idx] = m.fvalue; // Direct indexed access
        }
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
  os_workgroup_join_token_t oswg_token = {0};
  bool oswg_joined = false;
  bool oswg_logged = false;
  bool oswg_warned = false;
#endif
  for (;;) {
    if (!atomic_load_explicit(&g_engine.runFlag, memory_order_acquire))
      break;

    // Park until a block is published
    wait_for_block_start_or_shutdown();
    if (!atomic_load_explicit(&g_engine.runFlag, memory_order_acquire))
      break;

    // Handle OS workgroup joining BEFORE checking for work session
    // This allows workers to join even when there's no work yet
#ifdef HAVE_OS_WORKGROUP
    if (!oswg_joined) {
      void *w = atomic_load_explicit(&g_engine.oswg, memory_order_acquire);
      if (w) {
        oswg = (os_workgroup_t)w;
        bool ok = os_workgroup_join(oswg, &oswg_token);
        oswg_joined = ok;
        if (ok) {
          printf("[audiograph] worker %p joined os_workgroup\n", (void *)pthread_self());
          // Decrement counter; last worker to reach 0 clears the flag
          int remaining =
            atomic_fetch_sub_explicit(&g_engine.oswg_join_remaining, 1,
                                    memory_order_acq_rel) -
            1;
          if (remaining == 0) {
            atomic_store_explicit(&g_engine.oswg_join_pending, 0,
                              memory_order_release);
          }
        } else {
        if (!oswg_logged) {
          fprintf(
              stderr,
              ok ? "[audiograph] worker %p joined os_workgroup %p\n"
                 : "[audiograph] worker %p FAILED to join os_workgroup %p\n",
              (void *)pthread_self(), (void *)oswg);
          oswg_logged = true;
        }
        }
      } else if (!oswg_warned) {
        fprintf(
            stderr,
            "[audiograph] os_workgroup pointer not set; workers not joined\n");
        oswg_warned = true;
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
    os_workgroup_leave(oswg, oswg_token);
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

  // Set counter to number of workers, then set flag and broadcast
  // This ensures all workers see the flag before it's cleared
  atomic_store_explicit(&g_engine.oswg_join_remaining, g_engine.workerCount,
                        memory_order_release);
  atomic_store_explicit(&g_engine.oswg_join_pending, 1, memory_order_release);
  pthread_mutex_lock(&g_engine.sess_mtx);
  pthread_cond_broadcast(&g_engine.sess_cv);
  pthread_mutex_unlock(&g_engine.sess_mtx);

  fprintf(stderr,
          "[audiograph] set os_workgroup=%p (waking %d workers to join now)\n",
          oswg_ptr, g_engine.workerCount);
#else
  (void)oswg_ptr;
  fprintf(
      stderr,
      "[audiograph] os_workgroup unsupported at compile time; ignoring set\n");
#endif
}

void engine_clear_os_workgroup(void) { engine_set_os_workgroup(NULL); }

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

  float *inPtrs[MAX_IO];
  float *outPtrs[MAX_IO];

  // Inputs: each port has 0/1 producer (edge id or -1)
  for (int i = 0; i < node->nInputs && i < MAX_IO; i++) {
    int eid = node->inEdgeId ? node->inEdgeId[i] : -1;
    if (eid >= 0 && eid < lg->edge_capacity && lg->edges[eid].buf) {
      inPtrs[i] = lg->edges[eid].buf;
    } else {
      inPtrs[i] = lg->silence_buf;
    }
  }

  // Outputs: one buffer per output port (edge id or -1)
  // CRITICAL FIX: Use thread-local scratch to prevent write-write races
  for (int i = 0; i < node->nOutputs && i < MAX_IO; i++) {
    int eid = node->outEdgeId ? node->outEdgeId[i] : -1;
    if (eid >= 0 && eid < lg->edge_capacity && lg->edges[eid].buf) {
      outPtrs[i] = lg->edges[eid].buf;
    } else {
      outPtrs[i] = get_tls_out_scratch(i, nframes);
    }
  }

  if (node->vtable.process) {
    node->vtable.process((float *const *)inPtrs, (float *const *)outPtrs,
                         nframes, node->state);
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
  // Notify successors (node-level)
  for (int i = 0; i < node->succCount; i++) {
    int succ = node->succ[i];
    if (succ < 0 || succ >= lg->node_count) {
      continue;
    }
    if (lg->is_orphaned[succ]) {
      continue;
    }
    if (atomic_fetch_sub_explicit(&lg->pending[succ], 1,
                                  memory_order_acq_rel) == 1) {
      rq_push_or_spin(lg->readyQueue, succ); // CRITICAL FIX: Never drop work
    }
  }

  atomic_fetch_sub_explicit(&lg->jobsInFlight, 1, memory_order_acq_rel);
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

static void init_pending_and_seed(LiveGraph *lg) {
  int totalJobs = 0;

  // CRITICAL FIX: Properly reset/drain the ready queue to prevent stale node
  // IDs
  rq_reset(lg->readyQueue);

  // pending = indegree for reachable nodes, -1 for orphaned/deleted
  for (int i = 0; i < lg->node_count; i++) {
    // Skip deleted nodes (no process fn AND no ports)
    bool deleted = (lg->nodes[i].vtable.process == NULL &&
                    lg->nodes[i].nInputs == 0 && lg->nodes[i].nOutputs == 0);
    if (deleted || lg->is_orphaned[i]) {
      atomic_store_explicit(&lg->pending[i], -1, memory_order_relaxed);
      continue;
    }

    int indeg = lg->indegree[i]; // maintained incrementally at edits
    atomic_store_explicit(&lg->pending[i], indeg, memory_order_relaxed);

    bool hasOut = node_has_any_output_connected(lg, i);
    bool isSink = !hasOut && indeg > 0; // if you have true sinks that must run

    if (hasOut || isSink) {
      totalJobs++;
      if (indeg == 0 && hasOut) {
        rq_push_or_spin(lg->readyQueue, i); // CRITICAL FIX: Never drop work
      }
    }
  }

  atomic_store_explicit(&lg->jobsInFlight, totalJobs, memory_order_release);
}

bool detect_cycle(LiveGraph *lg) {
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
    return;
  }

  // check if no work to be done
  if (atomic_load_explicit(&lg->jobsInFlight, memory_order_acquire) <= 0)
    return;


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
    while (atomic_load_explicit(&lg->jobsInFlight, memory_order_acquire) > 0) {
      if (rq_try_pop(lg->readyQueue, &nid)) {
        execute_and_fanout(lg, nid, nframes);
      } else {
        cpu_relax(); // Brief pause when no work available
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
}

int find_live_output(LiveGraph *lg) {
  return lg->dac_node_id; // Simply return the DAC node - no searching needed
}

// Forward declaration for watch list function
static void update_watched_node_states(LiveGraph *lg);

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
