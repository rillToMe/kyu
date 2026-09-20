// ============================================================
// Gen12 completion fence — AL-7
// (graphics/backend/intel/intel_gen12_fence.c)
//
// Own status page @ GEN12_STATUS_GPUADDR, own seqno, own lock.
// GPU signals via MI_STORE_DWORD_IMM (header encoding is
// generation-stable MI — constants shared, legacy code untouched).
// Wait discipline mirrors Phase-13/14: bounded poll, hlt-sleep
// when IF allows (timer IRQ wakes), pause-spin otherwise; never
// sleep under a lock (lock covers seqno alloc only, a few insns).
// Timeout (-1) means "use CPU fallback", never a fatal error.
// ============================================================

#include "intel_gen12_fence.h"
#include "intel_gen12.h"
#include "intel_gen12_batch.h"
#include "intel_regs.h"
#include "intel_gtt.h"
#include "spinlock.h"
#include "pmm.h"
#include <string.h>

extern intel_gen_t intel_get_generation(void);
extern void serial_print(const char* s);
extern uint64_t hhdm_offset;

static volatile uint32_t* g_status_cpu = 0;
static phys_addr_t g_status_phys = PHYS_NULL;
static int g_fence_ok = 0;
static uint32_t g_next_seq = 1;
static spinlock_t g_fence_lock = SPINLOCK_INIT;

#define FENCE_WRAP_AT 0xFFFFFFFEu
#define FENCE_DEFAULT_SPINS 1000000u

uint64_t gen12_fence_status_addr(void) {
    return g_fence_ok ? GEN12_STATUS_GPUADDR : 0;
}

int gen12_fence_init(void) {
    if (g_fence_ok) return 0;
    if (intel_get_generation() != INTEL_GEN12) {
        serial_print("[gen12_fence] SKIP (not Gen12)\n");
        return -1;
    }
    if (!intel_gtt_is_active()) {
        serial_print("[gen12_fence] SKIP (GTT unavailable)\n");
        return -1;
    }
    if (intel_gen12_vm_valid(GEN12_STATUS_GPUADDR, 4096) != 0) {
        serial_print("[gen12_fence] FAIL (reservation outside GGTT)\n");
        return -1;
    }
    g_status_phys = pmm_alloc_page();
    if (g_status_phys == PHYS_NULL) {
        serial_print("[gen12_fence] SKIP (no PMM page)\n");
        return -1;
    }
    if (intel_gtt_map_pages(GEN12_STATUS_GPUADDR, &g_status_phys, 1)
        == (uint32_t)-1) {
        pmm_free_page(g_status_phys);
        g_status_phys = PHYS_NULL;
        serial_print("[gen12_fence] FAIL (GGTT map)\n");
        return -1;
    }
    g_status_cpu = (volatile uint32_t*)(g_status_phys + hhdm_offset);
    *g_status_cpu = 0;
    g_next_seq = 1;
    g_fence_ok = 1;
    return 0;
}

int gen12_fence_alloc(gen12_fence_t* f) {
    if (!f || !g_fence_ok) return -1;
    uint64_t flags = spinlock_lock_irqsave(&g_fence_lock);
    uint32_t s = g_next_seq;
    if (s >= FENCE_WRAP_AT) {
        // Synchronous use: no fence older than this can be in flight
        // (every HW op waits before returning), so restarting is safe.
        *g_status_cpu = 0;
        s = 1;
    }
    g_next_seq = s + 1;
    spinlock_unlock_irqrestore(&g_fence_lock, flags);
    f->seqno = s;
    return 0;
}

// Append MI_STORE_DWORD_IMM(status_addr, seqno). Same 4-DW shape the
// legacy fence uses — the opcode is generation-stable MI. Encoding only:
// works on scratch without init (AL-8 requires init before submit).
//
// AL-8 correction: bit22 (MI_USE_GGTT, intel_gpu_commands.h) MUST be set.
// The status page is GGTT-mapped and the context programs no PPGTT roots,
// so a PPGTT-addressed STORE would fault. (i915 breadcrumbs always set it;
// cf. setup_predicate_disable_wa in intel_lrc.c.)
int gen12_fence_emit(void* batch, const gen12_fence_t* fence) {
    gen12_batch_t* b = (gen12_batch_t*)batch;
    if (!b || !b->cpu || b->sealed || !fence || fence->seqno == 0) return -1;
    if (b->n + 4 > GEN12_BATCH_DWORDS) return -1;
    b->cpu[b->n++] = INTEL_MI_STORE_DW_IMM | (INTEL_MI_STORE_LEN - 2)
                     | (1u << 22);   // MI_USE_GGTT
    b->cpu[b->n++] = (uint32_t)(GEN12_STATUS_GPUADDR & 0xFFFFFFFFu);
    b->cpu[b->n++] = (uint32_t)(GEN12_STATUS_GPUADDR >> 32);
    b->cpu[b->n++] = fence->seqno;
    return 0;
}

int gen12_fence_is_signaled(const gen12_fence_t* f) {
    if (!f || f->seqno == 0 || !g_fence_ok) return 0;
    return *g_status_cpu >= f->seqno;
}

static int irq_enabled(void) {
    uint64_t rf;
    __asm__ volatile("pushfq; pop %0" : "=r"(rf));
    return (rf & (1ULL << 9)) != 0;
}

int gen12_fence_wait(const gen12_fence_t* f, uint32_t max_spins) {
    if (!f || f->seqno == 0 || !g_fence_ok) return -1;
    if (max_spins == 0) max_spins = FENCE_DEFAULT_SPINS;
    int can_hlt = irq_enabled();
    for (uint32_t i = 0; i < max_spins; i++) {
        if (gen12_fence_is_signaled(f)) return 0;
        if ((i & 0xFF) == 0) {
            if (can_hlt) {
                __asm__ volatile("sti");
                __asm__ volatile("hlt");
            } else {
                for (volatile int p = 0; p < 64; p++)
                    __asm__ volatile("pause");
            }
        }
    }
    return gen12_fence_is_signaled(f) ? 0 : -1;
}

static int g_fence_tested = 0;

void gen12_fence_selftest(void) {
    if (g_fence_tested) return;
    g_fence_tested = 1;

    // Pure: STORE encoding on scratch (no mapping needed).
    {
        static uint32_t scratch[16];
        gen12_batch_t t;
        t.cpu = scratch; t.phys = 0; t.gpu = 0;
        t.n = 0; t.sealed = 0; t.backed = 0;
        gen12_fence_t f;
        f.seqno = 0;
        if (gen12_fence_emit(&t, &f) == 0) goto fail;   // seqno 0 refused
        f.seqno = 41;
        if (gen12_fence_emit(&t, &f) != 0) goto fail;
        if (t.n != 4) goto fail;
        if (scratch[0] != (INTEL_MI_STORE_DW_IMM |
                           (INTEL_MI_STORE_LEN - 2) | (1u << 22))) goto fail;
        if (scratch[1] != (uint32_t)GEN12_STATUS_GPUADDR) goto fail;
        if (scratch[2] != 0) goto fail;
        if (scratch[3] != 41) goto fail;
        // Invalid fence never signals, wait always fails.
        gen12_fence_t bad;
        bad.seqno = 0;
        if (gen12_fence_is_signaled(&bad)) goto fail;
        if (gen12_fence_wait(&bad, 10) == 0) goto fail;
    }

    // Live: needs the mapped status page.
    if (gen12_fence_init() != 0) {
        serial_print("[gen12_fence] SKIP live (fence unavailable)\n");
        serial_print("[gen12_fence] fence: PASS (encoding)\n");
        return;
    }
    gen12_fence_t a, b;
    if (gen12_fence_alloc(&a) != 0) goto fail;
    if (gen12_fence_alloc(&b) != 0) goto fail;
    if (!(b.seqno > a.seqno)) goto fail;              // monotonic
    *g_status_cpu = a.seqno;                          // fake GPU completion
    if (!gen12_fence_is_signaled(&a)) goto fail;
    if (gen12_fence_is_signaled(&b)) goto fail;       // b still pending
    if (gen12_fence_wait(&a, 1000) != 0) goto fail;
    if (gen12_fence_wait(&b, 1000) == 0) goto fail;   // timeout → fallback
    *g_status_cpu = b.seqno;
    if (gen12_fence_wait(&b, 1000) != 0) goto fail;

    serial_print("[gen12_fence] fence: PASS\n");
    return;
fail:
    serial_print("[gen12_fence] fence: FAIL\n");
}
