// ============================================================
// Gen12 fault / timeout handling — AL-17
// (graphics/backend/intel/intel_gen12_fault.c)
//
// Needs the fence status page (Gen12+GTT); without it there is
// no live state to fault, so SKIP (the AL-16 suite already
// covers every dead-engine door). Every wait here uses a SMALL
// bound: a dead engine must cost milliseconds, never boot time.
// After each induced failure the suite performs a valid op —
// "the system remains alive" is demonstrated, not asserted.
//
// Failure model (roadmap): Gen12 unavailable → Intel accel
// disabled (live stays 0) → GHAL fallback → software backend.
// Graphics failure never panics: audit — no unbounded loop in
// any gen12 file (all polls carry counters), no div-by-zero
// (bench divisors are const nonzero), no asserts, no halt.
// ============================================================

#include "intel_gen12_fault.h"
#include "intel_gen12.h"
#include "intel_gen12_batch.h"
#include "intel_gen12_fence.h"
#include "intel_gen12_submit.h"
#include "intel_gen12_ctx.h"

extern void serial_print(const char* s);

static int g_fault_done = 0;

void gen12_fault_selftest(void) {
    if (g_fault_done) return;
    g_fault_done = 1;

    // Gate: need a mapped status page for timeout tests.
    if (gen12_fence_status_addr() == 0) {
        serial_print("[gen12_fault] SKIP (no fence state)\n");
        return;
    }

    // 1. Fence timeout is bounded and returns.
    gen12_fence_t f1;
    if (gen12_fence_alloc(&f1) != 0) {
        serial_print("[gen12_fault] SKIP (no fence alloc)\n");
        return;
    }
    if (gen12_fence_wait(&f1, 1000) == 0) {
        // Engine completed it without a submit: only possible if a
        // previous STORE already passed this seqno — still bounded.
        serial_print("[gen12_fault] timeout: already-signaled (bounded)\n");
    } else {
        serial_print("[gen12_fault] timeout: bounded return\n");
    }

    // 2. System alive after the timeout: fresh fence, higher seqno,
    // valid STORE encoding into scratch.
    gen12_fence_t f2;
    if (gen12_fence_alloc(&f2) != 0) {
        serial_print("[gen12_fault] FAIL (alloc after timeout)\n");
        return;
    }
    if (!(f2.seqno > f1.seqno)) {
        serial_print("[gen12_fault] FAIL (seqno stuck)\n");
        return;
    }
    {
        static uint32_t scratch[8];
        gen12_batch_t b;
        b.cpu = scratch; b.phys = 0; b.gpu = 0;
        b.n = 0; b.sealed = 0; b.backed = 0;
        if (gen12_fence_emit(&b, &f2) != 0 ||
            gen12_batch_end(&b) != 0 || b.n != 5) {
            serial_print("[gen12_fault] FAIL (encode after timeout)\n");
            return;
        }
    }
    serial_print("[gen12_fault] alive: PASS\n");

    // 3. Submit while the engine may be dead: must return (PASS if
    // the GPU actually executed, BLOCKED if not) — never hang.
    {
        uint32_t dw[5];
        dw[0] = (0x20u << 23) | 2u | (1u << 22);
        dw[1] = (uint32_t)(GEN12_STATUS_GPUADDR & 0xFFFFFFFFu);
        dw[2] = 0;
        dw[3] = f2.seqno;
        dw[4] = (0x0Au << 23);
        // Commit needs ring+ctx; absent here ⇒ clean -1 (also bounded).
        int sc = gen12_submit_commit(dw, 5);
        if (sc == 0) {
            if (gen12_fence_wait(&f2, 5000) == 0)
                serial_print("[gen12_fault] submit-while-dead: PASS\n");
            else
                serial_print("[gen12_fault] submit-while-dead: BLOCKED\n");
        } else {
            serial_print("[gen12_fault] submit-while-dead: BLOCKED\n");
        }
    }

    // 4. Invalid engine / context stays rejected.
    if (!g_gen12_engines.copy_present) {
        if (intel_gen12_ctx_lrca() != 0) {
            serial_print("[gen12_fault] FAIL (ctx without engine)\n");
            return;
        }
        serial_print("[gen12_fault] invalid-engine: rejected\n");
    } else {
        serial_print("[gen12_fault] invalid-engine: n/a (engine present)\n");
    }

    // 5. End state: unusable HW ⇒ live==0 ⇒ GHAL reports
    // acceleration-disabled and the CPU paths serve. (The GHAL-side
    // half is observed by the AL-14 check after ghal_init.)
    if (!gen12_is_live())
        serial_print("[gen12_fault] accel: disabled (fallback holds)\n");
    else
        serial_print("[gen12_fault] accel: live\n");

    serial_print("[gen12_fault] fault: PASS\n");
}
