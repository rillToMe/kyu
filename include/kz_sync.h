#ifndef KZ_SYNC_H
#define KZ_SYNC_H

#include <stdint.h>
#include "kz_surface.h"

// ============================================================
// R1 Synchronization + Present Contract (include/kz_sync.h)
//
// Three completion stages are NEVER conflated:
//
//   1. RENDER_DONE   - every producer write for this submission landed.
//   2. CONSUMER_DONE - every reader (compositor/backend) finished reading.
//   3. RELEASED      - the buffer may be reused without data race.
//
// Ordering is total: SUBMITTED < RENDER_DONE < CONSUMER_DONE < RELEASED.
// Sync (software) backends complete all three immediately at submit.
// Async (VirtIO) backends advance via the existing GHAL fence trio:
//   present_fence() -> fence_pending() poll -> fence_wait() bounded,
// and a buffer is reusable only when !fence_pending (see kz_fence_released).
//
// Mapping onto GHAL today:
//   submission accepted  = ghal_present_checked() returns 0
//   render done (virtio)= TRANSFER + FLUSH both complete (fence_done)
//   consumer done        = flush retired AND watermark advanced
//   released             = !ghal_fence_pending(fence)
// A failed submit (<0) accepts NOTHING: damage stays dirty (retry later).
//
// Hard rules:
//   - No unbounded wait in a graphics hot path. Every wait takes a bounded
//     budget (KZ_FENCE_WAIT_*); KZ_FENCE_WAIT_INFINITE does not exist.
//   - A timeout NEVER implies safe reuse. On timeout the buffer enters
//     quarantine (stay IN_USE, keep polling or reset the device); only an
//     observed !pending releases it.
//   - Never report async work successful merely because it was submitted.
//   - Never wait on the GPU while holding an unrelated global lock.
// ============================================================

typedef enum {
    KZ_STAGE_SUBMITTED = 0,  // accepted by the backend, nothing proven
    KZ_STAGE_RENDER_DONE,    // producer writes complete
    KZ_STAGE_CONSUMER_DONE,  // readers finished
    KZ_STAGE_RELEASED,       // safe to reuse
} kz_stage_t;

// Wait budgets in microseconds. Bounded by construction; there is no
// infinite-wait constant on purpose.
#define KZ_FENCE_POLL_US      1000u      // one non-blocking poll slice
#define KZ_FENCE_WAIT_MAX_US  500000u    // 0.5 s hard ceiling per wait call

// Fence handle: backend-issued id plus the stage it proves. A fence proves
// exactly its stage and every stage before it, never a later one.
typedef struct {
    uint64_t id;       // backend fence id (0 = none / sync-completed)
    uint32_t stage;    // kz_stage_t this fence proves
    uint32_t owner;    // issuer tag (backend id)
} kz_fence_t;

// Stage order check: reached >= expected means proven. Pure.
static inline int kz_stage_reached(kz_stage_t reached, kz_stage_t expected) {
    return (int)reached >= (int)expected;
}

// A sync (software) submit completes every stage immediately.
static inline kz_stage_t kz_stage_sync_submit(void) {
    return KZ_STAGE_RELEASED;
}

// Release check for async backends: reusable only when the device reports
// the fence no longer pending. `pending` is ghal_fence_pending(fence).
static inline kz_status_t kz_fence_released(int pending) {
    return pending ? KZ_EBUSY : KZ_OK;
}

// Validate a wait budget: must be > 0 and within the hard ceiling.
static inline kz_status_t kz_wait_validate(uint32_t budget_us) {
    if (budget_us == 0 || budget_us > KZ_FENCE_WAIT_MAX_US) return KZ_EINVAL;
    return KZ_OK;
}

#endif
