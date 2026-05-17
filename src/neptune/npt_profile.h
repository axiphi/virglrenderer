/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Host-side perf/debug logging.  All NPT-* log lines emitted by the
 * host live in npt_profile.c; per-flag semantics are documented at
 * the enum in npt_common.h.
 */

#ifndef NPT_PROFILE_H
#define NPT_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "util/list.h"
#include "util/macros.h"
#include "c11/threads.h"

struct npt_ring;

#define NPT_PROFILE_NUM_SLOTS 1024u

/* Per-ring counters (written only by the owning ring thread; no atomics
 * needed).  The dump walker aggregates across all registered rings. */
struct npt_profile_ring {
   uint64_t dispatches;
   uint64_t dispatch_ns;
   uint64_t relax_ns;
   uint64_t relax_calls;
   uint64_t idle_waits;
   uint64_t idle_wait_ns;
   uint64_t read_bytes;
};

struct npt_profile_slot {
   uint32_t cmd_type;
   uint32_t count;
   uint64_t dispatch_ns;
   uint64_t lookup_ns;
};

struct npt_profile {
   bool enabled;
   uint64_t period_ns;

   mtx_t rings_mutex;
   struct list_head rings;

   /* Process-global because object_mutex is shared across rings. */
   uint64_t lookups;
   uint64_t lookup_ns;
   uint64_t lookup_misses;

   struct npt_profile_slot slots[NPT_PROFILE_NUM_SLOTS];

   uint64_t last_dump_ns;
};

extern struct npt_profile npt_profile;

void npt_profile_init(void);
uint64_t npt_profile_now_ns(void);
void npt_profile_dump(const char *reason);

void npt_profile_register_ring(struct npt_ring *ring);
void npt_profile_unregister_ring(struct npt_ring *ring);

/* Per-(push, pop) pair info populated by the pop helpers under
 * NPT_DEBUG=fence_trace and handed to npt_profile_log_pd_pop so the
 * trace line can join push and pop in one log entry with the
 * fence_id the pop is being matched against. */
struct npt_pop_info {
   uint64_t push_seq;
   uint64_t frame_id;
   uint32_t image_index;
   uint64_t push_to_pop_us;
   uint64_t pop_block_us;
};

/* Emit one NPT-PD-PUSH line per onPresentSubmitted push.  push_t_ns
 * is the CLOCK_MONOTONIC ns timestamp stamped on the entry at push
 * time; depth is the FIFO depth after insertion. */
void npt_profile_log_pd_push(uint32_t ctx_id, uint64_t push_seq,
                             uint64_t frame_id, uint32_t image_index,
                             int sync_fd, uint64_t push_t_ns,
                             uint32_t depth);

/* Emit one NPT-PD-POP line per pop.  source distinguishes the two
 * pop paths: "try" = submit_fence's non-blocking pop, "wait" = the
 * queue worker's blocking pop after a deferred submit_fence. */
void npt_profile_log_pd_pop(uint32_t ctx_id,
                            const struct npt_pop_info *info,
                            int sync_fd, uint32_t ring_idx,
                            uint64_t fence_id, const char *source);

/* Emit one NPT-Q-RETIRE line per fence the queue worker retires.
 * poll_us is the wall-time spent in npt_wait_sync_fd; rc is its
 * return code (>0 signalled, 0 timeout, <0 error). */
void npt_profile_log_q_retire(uint32_t ring_idx, uint64_t fence_id,
                              int sync_fd, uint64_t poll_us, int rc);

static inline bool
npt_profile_enabled(void)
{
   return npt_profile.enabled;
}

static inline void
npt_profile_record_dispatch(struct npt_profile_ring *p, uint32_t cmd_type,
                            uint64_t dispatch_ns)
{
   if (!npt_profile.enabled)
      return;

   p->dispatches++;
   p->dispatch_ns += dispatch_ns;

   uint32_t h = cmd_type;
   h = (h ^ (h >> 16)) * 0x85ebca6bu;
   h = (h ^ (h >> 13)) * 0xc2b2ae35u;
   h = (h ^ (h >> 16)) & (NPT_PROFILE_NUM_SLOTS - 1u);
   for (uint32_t i = 0; i < NPT_PROFILE_NUM_SLOTS; i++) {
      uint32_t idx = (h + i) & (NPT_PROFILE_NUM_SLOTS - 1u);
      struct npt_profile_slot *s = &npt_profile.slots[idx];
      if (s->cmd_type == 0 || s->cmd_type == cmd_type) {
         s->cmd_type = cmd_type;
         s->count++;
         s->dispatch_ns += dispatch_ns;
         return;
      }
   }
}

static inline void
npt_profile_record_lookup(uint64_t lookup_ns, bool miss, uint32_t cmd_type)
{
   if (!npt_profile.enabled)
      return;
   npt_profile.lookups++;
   npt_profile.lookup_ns += lookup_ns;
   if (miss)
      npt_profile.lookup_misses++;

   if (!cmd_type)
      return;
   uint32_t h = cmd_type;
   h = (h ^ (h >> 16)) * 0x85ebca6bu;
   h = (h ^ (h >> 13)) * 0xc2b2ae35u;
   h = (h ^ (h >> 16)) & (NPT_PROFILE_NUM_SLOTS - 1u);
   for (uint32_t i = 0; i < NPT_PROFILE_NUM_SLOTS; i++) {
      uint32_t idx = (h + i) & (NPT_PROFILE_NUM_SLOTS - 1u);
      struct npt_profile_slot *s = &npt_profile.slots[idx];
      if (s->cmd_type == cmd_type) {
         s->lookup_ns += lookup_ns;
         return;
      }
      if (s->cmd_type == 0)
         return;
   }
}

static inline void
npt_profile_record_relax(struct npt_profile_ring *p, uint64_t relax_ns)
{
   if (!npt_profile.enabled)
      return;
   p->relax_ns += relax_ns;
   p->relax_calls++;
}

static inline void
npt_profile_record_idle_wait(struct npt_profile_ring *p, uint64_t wait_ns)
{
   if (!npt_profile.enabled)
      return;
   p->idle_waits++;
   p->idle_wait_ns += wait_ns;
}

static inline void
npt_profile_record_read(struct npt_profile_ring *p, uint32_t bytes)
{
   if (npt_profile.enabled)
      p->read_bytes += bytes;
}

static inline void
npt_profile_maybe_dump(struct npt_profile_ring *p)
{
   if (!npt_profile.enabled)
      return;
   if (likely(p->dispatches & 0xFFFu))
      return;
   uint64_t now = npt_profile_now_ns();
   if (now - npt_profile.last_dump_ns >= npt_profile.period_ns) {
      npt_profile.last_dump_ns = now;
      npt_profile_dump("periodic");
   }
}

#endif /* NPT_PROFILE_H */
