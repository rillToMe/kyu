#ifndef INTEL_GEN12_FENCE_H
#define INTEL_GEN12_FENCE_H

#include <stdint.h>

// ============================================================
// Gen12 completion fence — AL-7
// (graphics/backend/intel_gen12_fence.h)
//
// Mechanism (same pattern i915 breadcrumbs use): the batch ends
// with MI_STORE_DWORD_IMM writing a seqno to a GGTT-mapped status
// page; signaled = memory value reached. Poll + hlt sleep, always
// bounded; timeout converges to CPU fallback (roadmap failure
// model). Legacy Phase-13 fence is frozen and untouched — this is
// a parallel Gen12-owned instance (own page, own seqno, own lock).
// ============================================================

// GGTT reservation for the Gen12 status page (1 page). Map so far:
//   0x00000 legacy ring | 0x10000 legacy status | 0x20000 gen12 ctx
//   0x24000 gen12 status | 0x100000+ bump area
#define GEN12_STATUS_GPUADDR 0x24000ULL

typedef struct {
    uint32_t seqno;   // 0 = invalid/unallocated
} gen12_fence_t;

// Map the status page (idempotent). 0 ok, -1 SKIP/FAIL (prints reason).
int gen12_fence_init(void);
// Allocate the next seqno (monotonic from 1; wraps safely). -1 when down.
int gen12_fence_alloc(gen12_fence_t* f);
// Append the completion STORE for f to an open batch (before BB_END).
// Batch stays unsealed; caller ends it. Needs gen12_batch_t — declared
// structurally here to avoid pulling the batch header into IRQ paths.
int gen12_fence_emit(void* batch, const gen12_fence_t* f);
// Non-zero when the GPU has written >= f.seqno (lock-free read).
int gen12_fence_is_signaled(const gen12_fence_t* f);
// Bounded wait: 0 signaled, -1 timeout (caller falls back to CPU).
// Never sleeps under a lock; never polls unbounded.
int gen12_fence_wait(const gen12_fence_t* f, uint32_t max_spins);
// GGTT address of the status page (for batch emission). 0 when down.
uint64_t gen12_fence_status_addr(void);
// CPU-only + live self-test. Prints PASS / SKIP / FAIL, never fatal.
void gen12_fence_selftest(void);

#endif // INTEL_GEN12_FENCE_H
