#ifndef INTEL_FENCE_H
#define INTEL_FENCE_H

// ============================================================
// Intel GPU fence / completion — Phase 13
//
// Synchronous only. intel_gpu_submit() appends MI_STORE_IMM
// (status page = last completed seqno) + BB_END to the staging
// buffer, submits via BCS, and returns a fence. Ring executes
// in order, so status >= seqno means the fence's work is done.
//
// No BCS (fallback): fence is CPU-backed and immediately
// signaled — CPU path needs no GPU tracking.
// Async / IRQ wake comes in Phase 14.
// ============================================================

#include <stdint.h>
#include "intel_cmd.h"

typedef struct {
    uint32_t seqno;
    uint8_t  hw;   // 1 = GPU-backed, 0 = CPU fallback (always signaled)
} intel_fence_t;

// Ensure status page + seqno space ready. 0 ok, -1 fail.
int intel_fence_init(void);

// Append STORE+BB_END, submit. Fence written to *out. 0 ok.
int intel_gpu_submit(intel_cmd_t* c, intel_fence_t* out);

// 1 = signaled, 0 = pending.
int intel_fence_is_signaled(const intel_fence_t* f);

// Spin until signaled or spins exhausted. 0 = signaled.
int intel_fence_wait(const intel_fence_t* f, uint32_t max_spins);

// Sleep until signaled: hlt (wake on any IRQ, e.g. timer tick)
// instead of spinning. IF-clear contexts fall back to pause.
// Calls intel_irq_handler() once completion is observed, so the
// observation path is shared with the future ISR vector.
// 0 = signaled.
int intel_fence_wait_sleep(const intel_fence_t* f, uint32_t max_wakeups);

// Round-trip test (HW when BCS live, semantics when not).
// 0 = PASS, 1 = SKIP? no — always PASS/FAIL. Never fatal.
int intel_fence_test(void);

#endif // INTEL_FENCE_H
