/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-renderer-context Neptune state.
 */

#ifndef NPT_CONTEXT_H
#define NPT_CONTEXT_H

#include "npt_common.h"
#include "npt_cs.h"
#include "npt_feedback.h"
#include "npt_renderer.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "virgl_resource.h"

struct npt_queue;

/* Server-created resource (e.g. swapchain dmabuf image) that the
 * guest later claims via create_blob. */
struct npt_pending_blob {
   uint64_t blob_id;
   enum virgl_resource_fd_type fd_type;
   int fd;
   uint64_t size;
};

/* Map/Unmap bookkeeping lives on the SHM blob (D3D11/12 resources have
 * no npt_resource entry); the wire ops carry shmem_res_id for lookup. */
struct npt_resource_map_state {
   void    *mapped_data;   /* host pointer returned by D3D11/12 Map */
   uint32_t row_pitch;
   uint32_t depth_pitch;
   uint32_t mapped_size;
   uint32_t access_flags;  /* NPT_MAP_ACCESS_* — decides copy direction */
   bool     is_mapped;
   bool     persistent;    /* D3D12 persistent map: no Unmap expected */
};

struct npt_resource {
   uint32_t res_id;

   enum virgl_resource_fd_type fd_type;
   bool iov_owned; /* data borrowed from IOV: don't munmap */

   union {
      int fd;          /* dma_buf / opaque */
      uint8_t *data;   /* shm */
   } u;

   size_t size;

   /* Only used for SHM resources acting as MAP/UNMAP transfer blobs.
    * Zero-initialised by calloc. */
   struct npt_resource_map_state map_state;
};

struct npt_context {
   uint32_t ctx_id;

   /* Caller-supplied tag for log lines (typically the guest process
    * name).  May be NULL. */
   char *debug_name;

   /* True when this context runs on a virgl_render_server worker
    * thread; false in the default worker-process mode. */
   bool on_worker_thread;

   npt_renderer_retire_fence_callback_type retire_fence;

   mtx_t ring_mutex;
   struct list_head rings;

   /* Watchdog-reporter thread, lazily started by CREATE_RING.  OR-sets
    * NPT_RING_STATUS_ALIVE_BIT on every monitored ring at a period
    * equal to the smallest monitor_report_period_us requested across
    * all rings. */
   struct {
      mtx_t mutex;
      cnd_t cond;
      thrd_t thread;
      atomic_bool started;
      atomic_uint report_period_us;
   } ring_monitor;

   /* Single-waiter park lot: block until ring->id reaches a target
    * seqno.  Ring thread signals on progress and on fatal teardown.
    *
    * .id is atomic so the per-command on_ring_seqno_update can fast-
    * path via lockless load when no waiter is installed (the steady-
    * state case).  Writes to .id always happen under the mutex; the
    * atomic just lets the producer poll without contending for it. */
   struct {
      mtx_t mutex;
      cnd_t cond;
      _Atomic uint64_t id;
      uint32_t seqno;
   } wait_ring;

   mtx_t resource_mutex;
   struct hash_table *resource_table;

   /* Object handle table.  Maps guest-visible object_id (host COM
    * pointer cast to uint64_t) to npt_object_type, so handle lookup
    * can reject ids the guest never obtained from Create* / QI.
    * Drained by COM_RELEASE and context_destroy. */
   mtx_t object_mutex;
   struct hash_table *object_table;

   mtx_t pending_blob_mutex;
   struct hash_table *pending_blob_table;  /* blob_id -> npt_pending_blob */

   /* Guest-supplied display hints from SET_PREFERRED_DISPLAY_INFO.
    * Consumed at CreateSwapChain time only; updates after creation do
    * not retroactively affect an existing swapchain.  0 in any field
    * means "no preference, host uses its default". */
   _Atomic uint32_t preferred_virgl_format;
   _Atomic uint32_t preferred_refresh_num;
   _Atomic uint32_t preferred_refresh_den;

   /* Indexed by guest-supplied ring_idx; entry 0 is unused (ring_idx
    * 0 means "retire on CPU timeline").  Lazily populated when the
    * first fence on a given ring_idx arrives.
    *
    * Partition:
    *   0                          : CPU timeline
    *   [1, NPT_EVENT_RING_BASE)   : Present fences (per-swapchain)
    *   [NPT_EVENT_RING_BASE, end) : Win32 event proxies
    *
    * NPT_EVENT_RING_BASE must equal (sync_queues count / 2) in
    * lockstep with the guest driver, or an event-ring fence routes
    * into the present-done FIFO wait and deadlocks the dispatch
    * thread. */
   mtx_t sync_queues_mutex;
   struct npt_queue *sync_queues[64];

   /* FIFO of GPU-done sync_file fds produced by the host swapchain's
    * present callback.  Steady-state depth 0 or 1; the cap guards
    * against runaway accumulation when the guest stops consuming.
    *
    * submit_fence does a non-blocking try-pop on the proxy dispatch
    * thread; on empty FIFO it submits a deferred queue entry and the
    * per-ring queue worker performs the unbounded cnd_wait
    * (off the BQL path).  This preserves 1:1 pairing between guest
    * and host frame index — a synchronous fall-through retire would
    * misalign every subsequent frame.  shutting_down breaks the
    * worker's wait on context teardown. */
   mtx_t       present_done_mutex;
   cnd_t       present_done_cond;
   bool        present_done_shutting_down;
   struct list_head present_done_fds;
   uint32_t    present_done_count;
   /* Per-context push counter for NPT_DEBUG=fence_trace: stamped on
    * each push, carried through the pop so a single trace line can
    * join (push, pop) and the guest-side NPT-PRESENT-TIMING #N. */
   _Atomic uint64_t present_done_push_seq;

   /* Win32 event HANDLE emulation: each proxy owns an eventfd handed
    * to the host D3D library as the HANDLE.  event_pending_arms
    * carries (ring_idx, dup_fd) triples; the next matching
    * submit_fence pops one and feeds dup_fd to the sync-queue worker. */
   mtx_t                 event_mutex;
   struct hash_table    *event_proxies;
   struct list_head      event_pending_arms;

   /* Per-object shmem slots (queries, fences, ...) the dispatch
    * thread writes when host state advances, so the guest reads
    * results locally instead of round-tripping.  See npt_feedback. */
   struct npt_feedback_state feedback;

   bool cs_fatal_error;
   struct npt_cs_encoder encoder;
   struct npt_cs_decoder decoder;

   /* dispatch.data = ctx; override functions recover the outer
    * struct via npt_context_from_dispatch. */
   struct npt_dispatch_context dispatch;

   struct list_head head;
};

struct npt_present_done_entry {
   int sync_fd;
   /* Recorded at push time so the matching pop can emit a trace line
    * naming the frame.  CLOCK_MONOTONIC ns; push_seq is the per-context
    * monotonic counter. */
   uint32_t image_index;
   uint64_t frame_id;
   uint64_t push_t_ns;
   uint64_t push_seq;
   struct list_head head;
};

/* Forward decl; full definition lives in npt_profile.h alongside the
 * helper that consumes it. */
struct npt_pop_info;

#define NPT_PRESENT_DONE_FIFO_MAX 16u

/* Half of sync_queues[] is Present rings, half is event proxies.
 * Must be (sizeof sync_queues / sizeof sync_queues[0]) / 2 in lockstep
 * with the guest driver. */
#define NPT_EVENT_RING_BASE 32u

struct npt_context *
npt_context_create(uint32_t ctx_id,
                   npt_renderer_retire_fence_callback_type retire_fence,
                   size_t debug_len,
                   const char *debug_name);

void
npt_context_destroy(struct npt_context *ctx);

bool
npt_context_submit_cmd(struct npt_context *ctx, const void *buffer, size_t size);

/* Peek one command header, route to transport dispatch (group 0) or
 * the generated protocol dispatcher otherwise.  Shared by context and
 * ring submit_cmd loops.  Returns false on group-0 with no handler
 * (decoder left fatal); otherwise true (caller checks get_fatal). */
bool
npt_context_dispatch_one_command(struct npt_context *ctx,
                                 struct npt_dispatch_context *dispatch,
                                 struct npt_cs_decoder *dec,
                                 struct npt_cs_encoder *enc);

/* ring_idx 0 retires immediately on the CPU timeline; otherwise
 * lazily creates an npt_queue and pushes a sync onto its worker
 * thread.  The wait target comes from the per-context present-done
 * FIFO (present rings) or a pending ARM_EVENT_FENCE (event rings). */
bool
npt_context_submit_fence(struct npt_context *ctx,
                         uint32_t flags,
                         uint32_t ring_idx,
                         uint64_t fence_id);

/* Takes ownership of \p sync_fd; closes it on overflow.  image_index
 * and frame_id are recorded on the entry for the fence_trace log line
 * that fires at pop time. */
void
npt_context_push_present_done(struct npt_context *ctx, int sync_fd,
                              uint32_t image_index, uint64_t frame_id);

/* Blocking pop intended for the npt_queue worker thread.  Returns -1
 * on context shutdown.  MUST NOT run on the proxy dispatch thread:
 * that path holds QEMU's BQL and stalling here freezes the VM.  The
 * dispatch thread instead uses a non-blocking try-pop and submits a
 * deferred queue item when the FIFO is empty; the queue worker then
 * calls this to wait.  If \p out_info is non-NULL it is populated with
 * the popped entry's trace fields. */
int
npt_context_wait_pop_present_done(struct npt_context *ctx,
                                  struct npt_pop_info *out_info);

bool
npt_context_create_resource(struct npt_context *ctx,
                            uint32_t res_id,
                            uint64_t blob_id,
                            uint64_t blob_size,
                            uint32_t blob_flags,
                            struct virgl_context_blob *out_blob);

bool
npt_context_import_resource(struct npt_context *ctx,
                            uint32_t res_id,
                            enum virgl_resource_fd_type fd_type,
                            int fd,
                            uint64_t size);

void
npt_context_destroy_resource(struct npt_context *ctx, uint32_t res_id);

static inline struct npt_resource *
npt_context_get_resource(struct npt_context *ctx, uint32_t res_id)
{
   mtx_lock(&ctx->resource_mutex);
   const struct hash_entry *entry =
      _mesa_hash_table_search(ctx->resource_table, &res_id);
   mtx_unlock(&ctx->resource_mutex);

   return likely(entry) ? entry->data : NULL;
}

/* Takes ownership of the fd. */
bool
npt_context_register_pending_blob(struct npt_context *ctx,
                                  uint64_t blob_id,
                                  enum virgl_resource_fd_type fd_type,
                                  int fd,
                                  uint64_t size);

/* Wake any wait_ring waiter on \p ring_id whose target seqno is
 * reached.  Called after each dispatched command. */
void
npt_context_on_ring_seqno_update(struct npt_context *ctx,
                                 uint64_t ring_id,
                                 uint32_t ring_seqno);

/* Mark the context fatal and wake any wait_ring waiter. */
void
npt_context_on_ring_fatal(struct npt_context *ctx);

/* Idempotent.  Tightens the reporting rate when called with a shorter
 * period than the current one.  Returns false if the thread can't start. */
bool
npt_context_ring_monitor_init(struct npt_context *ctx,
                              uint32_t report_period_us);

/* Block until ring->id advances past ring_seqno or the context goes
 * fatal.  Returns false on cnd_wait failure; true on normal progress
 * AND on fatal-observed exit. */
struct npt_ring;
bool
npt_context_wait_ring_seqno(struct npt_context *ctx,
                            struct npt_ring *ring,
                            uint32_t ring_seqno);

/* Idle-path probe.  True (and writes *out_seqno) when the context is
 * currently waiting on ring_id; used by the ring thread to detect a
 * driver-supplied seqno the ring can never reach. */
bool
npt_context_get_wait_ring_seqno(struct npt_context *ctx,
                                uint64_t ring_id,
                                uint32_t *out_seqno);

static inline struct npt_context *
npt_context_from_dispatch(struct npt_dispatch_context *dispatch)
{
   return dispatch->data;
}

/* Object handle table.  Without it a compromised guest could pass an
 * arbitrary uint64_t and have the host deref it as a COM pointer. */

/* Safe with obj == NULL or type == 0.  Re-register with the same
 * type is a no-op; type upgrade (parent -> child) is allowed;
 * unrelated types log and keep the existing entry. */
void
npt_context_register_object(struct npt_context *ctx,
                            uint64_t id,
                            void *obj,
                            npt_object_type type);

void
npt_context_unregister_object(struct npt_context *ctx, uint64_t id);

/* Returns the pointer when the registered type is compatible with
 * `expected` (exact match, or either is an ancestor of the other;
 * IUNKNOWN matches anything).  On miss / type-mismatch logs the
 * violation and returns NULL; non-permissive lookups also mark the
 * decoder fatal so the ring tears down.  IUNKNOWN-expected lookups
 * are permissive: a missing id returns NULL silently to accommodate
 * the COM_RELEASE-vs-Create race. */
void *
npt_context_lookup_object(struct npt_context *ctx,
                          struct npt_cs_decoder *dec,
                          uint64_t id,
                          npt_object_type expected);

/* COM_RELEASE coordination: drop the feedback entry (if any), unmap
 * the object_table entry, then drop the host-library ref — either by
 * destroying the swapchain wrapper that owns it or by a plain
 * IUnknown::Release.  Stray RELEASE on an unregistered id is silent. */
void
npt_context_release_object(struct npt_context *ctx, uint64_t guest_id);

/* COM_QUERY_INTERFACE: resolve src_guest_id, call QI for riid, and on
 * success register the returned interface under new_guest_id.
 * Returns the host HRESULT (or NPT_E_NOINTERFACE if src_guest_id is
 * not registered). */
HRESULT
npt_context_query_interface(struct npt_context *ctx,
                            uint64_t src_guest_id,
                            const GUID *riid,
                            uint64_t new_guest_id);

#endif /* NPT_CONTEXT_H */
