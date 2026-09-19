// ============================================================
// Intel GPU fence / completion — Phase 13
// (graphics/backend/intel_fence.c)
//
// Monotonic seqnos from 1 (0 = reset state, never issued).
// Wrap: at 0xFFFFFFFE the status page is cleared and seqnos
// restart at 1. Safe because all use is synchronous — no
// outstanding fence can exist across the wrap.
// ============================================================

#include "intel_fence.h"
#include "intel_bcs.h"
#include "intel_irq.h"
#include "spinlock.h"

extern void serial_print(const char* s);

static uint32_t   g_next_seqno = 1;
static spinlock_t g_fence_lock = SPINLOCK_INIT;
static int        g_fence_ok = 0;

int intel_fence_init(void) {
    if (g_fence_ok) return 0;
    if (intel_cmd_status_init() != 0) return -1;
    intel_cmd_status_write(0);
    g_next_seqno = 1;
    g_fence_ok = 1;
    return 0;
}

int intel_gpu_submit(intel_cmd_t* c, intel_fence_t* out) {
    if (!c || !out || c->n == 0) return -1;
    if (!intel_bcs_is_available()) {
        // CPU fallback: nothing for the GPU to track.
        out->seqno = 0;
        out->hw = 0;
        return 0;
    }
    uint64_t flags = spinlock_lock_irqsave(&g_fence_lock);
    if (g_next_seqno >= 0xFFFFFFFE) {
        intel_cmd_status_write(0);
        g_next_seqno = 1;
    }
    uint32_t seq = g_next_seqno++;
    spinlock_unlock_irqrestore(&g_fence_lock, flags);

    if (intel_cmd_emit_store(c, intel_cmd_status_gpu_addr(), seq)) return -1;
    if (intel_cmd_emit_bb_end(c)) return -1;
    if (intel_cmd_submit(c)) return -1;
    out->seqno = seq;
    out->hw = 1;
    return 0;
}

int intel_fence_is_signaled(const intel_fence_t* f) {
    if (!f) return 0;
    if (!f->hw) return 1;
    // In-order ring: status holds the last completed seqno.
    return intel_cmd_status_read() >= f->seqno;
}

int intel_fence_wait(const intel_fence_t* f, uint32_t max_spins) {
    if (!f) return -1;
    if (!f->hw) return 0;
    if (max_spins == 0) max_spins = 1000000;
    for (uint32_t i = 0; i < max_spins; i++) {
        if (intel_fence_is_signaled(f)) return 0;
    }
    return -1;
}

static int irqs_enabled(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    return (flags & (1ULL << 9)) != 0;
}

int intel_fence_wait_sleep(const intel_fence_t* f, uint32_t max_wakeups) {
    if (!f) return -1;
    if (!f->hw) return 0;
    if (max_wakeups == 0) max_wakeups = 1000; // ~10s at 100Hz tick
    for (uint32_t i = 0; i < max_wakeups; i++) {
        if (intel_fence_is_signaled(f)) {
            intel_irq_handler(); // observe completion (also the ISR body)
            return 0;
        }
        if (irqs_enabled()) {
            __asm__ volatile("sti; hlt" ::: "memory");
        } else {
            for (volatile int s = 0; s < 1000; s++) __asm__ volatile("pause");
        }
    }
    return intel_fence_is_signaled(f) ? 0 : -1;
}

int intel_fence_test(void) {
    if (intel_fence_init() != 0) {
        serial_print("[intel_fence] FENCE: FAIL (init)\n");
        return -1;
    }
    // CPU-backed fence must read signaled immediately.
    intel_fence_t cpu = {0, 0};
    if (!intel_fence_is_signaled(&cpu) || intel_fence_wait(&cpu, 0) != 0) {
        serial_print("[intel_fence] FENCE: FAIL (cpu-semantics)\n");
        return -1;
    }
    if (!intel_bcs_is_available()) {
        serial_print("[intel_fence] FENCE: SKIP (no BCS)\n");
        return 1;
    }
    // HW round-trip: NOOP, submit, wait, verify status advanced.
    intel_cmd_t c;
    intel_cmd_begin(&c);
    if (intel_cmd_emit_noop(&c)) goto fail;
    intel_fence_t f;
    if (intel_gpu_submit(&c, &f)) goto fail;
    if (!f.hw || f.seqno == 0) goto fail;
    if (intel_fence_wait_sleep(&f, 0)) goto fail;
    if (!intel_fence_is_signaled(&f)) goto fail;
    // Handler observed completion through the sleep-wait path.
    if (intel_irq_last_seqno() < f.seqno) goto fail;
    serial_print("[intel_fence] FENCE: PASS\n");
    return 0;
fail:
    serial_print("[intel_fence] FENCE: FAIL\n");
    return -1;
}
