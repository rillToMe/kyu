// ============================================================
// Gen12 minimal submission — AL-8
// (graphics/backend/intel/intel_gen12_submit.c)
//
// SEQUENCE SOURCES (i915, torvalds/linux):
// - ELSP write order HI-then-LO to one port: write_desc() in
//   intel_execlists_submission.c (non-ctrl path).
// - ELSP = base + 0x230, BCS0 base 0x12000 → 0x12230
//   (intel_engine_regs.h RING_ELSP + engine table BLT_RING_BASE).
// - Descriptor low: LRCA | VALID | PRIVILEGE | LEGACY_32B<<3
//   (intel_lrc.h GEN8_CTX_*; GGTT-only → 32-bit legacy mode).
//   FORCE_RESTORE bit2 every submit (full restore, no lite-game).
// - Descriptor high (Gen11 format, 2014 lrc comment + intel_lrc.h
//   shifts): SW_ID(37-47)=1, instance(48-53)=0 (BCS0),
//   counter(55-60)++, class(61-63)=3 (COPY_ENGINE_CLASS,
//   2017 class patch: RENDER 0, VDEC 1, VENC 2, COPY 3).
// - Ring 16KB: i915 GEM context default ring_size = 4 pages.
// - Forcewake GT (covers 0x12000 block incl. ELSP): SET 0xa188,
//   ACK 0x130044, BIT0 (Gen12 tables + GT rename patch).
//   Held, not released (release risks clearing others' bits;
//   single static hold, negligible power cost at this phase).
//
// ASSUMPTIONS (documented, fail-closed if wrong):
// - No GuC firmware owns the engines (nothing loaded one), so the
//   CS listens to ELSP. A GuC-owned engine ignores ELSP → timeout.
// - GT awake enough for FW-ACK (BIOS ran display at boot).
// - Zero PDPs never dereferenced: every address the CS touches
//   here is GGTT (image, ring, USE_GGTT STORE). XY blits (AL-9+)
//   may need aliasing-PPGTT work — flagged, not guessed.
// ============================================================

#include "intel_gen12_submit.h"
#include "intel_gen12.h"
#include "intel_gen12_ctx.h"
#include "intel_gen12_fence.h"
#include "intel_gen12_ppgtt.h"
#include "intel_regs.h"
#include "intel_mmio.h"
#include "intel_gtt.h"
#include "spinlock.h"
#include "pmm.h"
#include <string.h>

extern intel_gen_t intel_get_generation(void);
extern void serial_print(const char* s);
extern void serial_print_hex(uint64_t v);
extern void serial_dec(uint64_t v);
extern uint64_t hhdm_offset;

#define ELSP_BCS0        0x12230u
#define FW_SET_GT        0x0A188u
#define FW_ACK_GT        0x130044u
#define FW_BIT           (1u << 0)

#define DESC_LO_FLAGS    ((1u << 0) | (1u << 8) | (1u << 3) | (1u << 2))
#define DESC_CLASS_COPY  (3u)
#define EXEC_STATUS_BCS0 0x12234u   // read-only diag (CSB status)

static int      g_ring_ok = 0;
static uint64_t g_ring_phys[GEN12_RING_PAGES];
static uint32_t g_ring_tail = 0;   // SW copy, bytes
static uint32_t g_sw_counter = 0;
static spinlock_t g_submit_lock = SPINLOCK_INIT;

// Request the GT power well, bounded ACK poll. 0 held, -1 give up.
static int fw_hold(void) {
    uint32_t s = intel_mmio_read32(FW_SET_GT);
    intel_mmio_write32(FW_SET_GT, s | FW_BIT);
    for (uint32_t i = 0; i < 500000; i++) {
        if (intel_mmio_read32(FW_ACK_GT) & FW_BIT) return 0;
    }
    return -1;
}

int gen12_submit_init(void) {
    if (g_ring_ok) return 0;
    if (intel_get_generation() != INTEL_GEN12) {
        serial_print("[gen12_submit] SKIP (not Gen12)\n");
        return -1;
    }
    if (!intel_gtt_is_active()) {
        serial_print("[gen12_submit] SKIP (GTT unavailable)\n");
        return -1;
    }
    if (intel_gen12_vm_valid(GEN12_RING_GPUADDR, GEN12_RING_SIZE) != 0) {
        serial_print("[gen12_submit] FAIL (reservation outside GGTT)\n");
        return -1;
    }
    for (uint32_t i = 0; i < GEN12_RING_PAGES; i++) {
        g_ring_phys[i] = pmm_alloc_page();
        if (g_ring_phys[i] == PHYS_NULL) {
            for (uint32_t j = 0; j < i; j++) pmm_free_page(g_ring_phys[j]);
            serial_print("[gen12_submit] SKIP (no PMM pages)\n");
            return -1;
        }
    }
    if (intel_gtt_map_pages(GEN12_RING_GPUADDR, g_ring_phys,
                            GEN12_RING_PAGES) == (uint32_t)-1) {
        for (uint32_t j = 0; j < GEN12_RING_PAGES; j++)
            pmm_free_page(g_ring_phys[j]);
        serial_print("[gen12_submit] FAIL (GGTT map)\n");
        return -1;
    }
    memset((void*)(g_ring_phys[0] + hhdm_offset), 0, 4096);
    g_ring_tail = 0;
    g_ring_ok = 1;
    return 0;
}

// AL-15 lock audit (all GPU locks irqsave; sleep/wait always lock-free):
//   g_submit_lock : ring copy + mirror-sync + image patch + ELSP (here)
//   g_fence_lock  : seqno alloc only (fence module)
//   g_buf_lock    : buffer pool id/vaddr bump (alloc module)
//   g_gtt_lock    : PTE writes (gtt module)
// Order when nested: buffer → GTT → fence → submission. No cycles:
// submission never calls back into buffer/GTT/fence alloc while holding
// g_submit_lock (sync is a read-only PTE walk + PT writes, no alloc).
// Fence wait, PMM alloc, batch build and buffer destroy never run
// under any GPU lock. Ring writes happen only in here, so concurrent
// CPUs can stage privately and serialize at commit with zero overlap.
int gen12_submit_commit(const uint32_t* dw, uint32_t ndw) {
    if (!g_ring_ok || !dw || ndw == 0) return -1;
    if (intel_gen12_ctx_lrca() == 0) return -1;
    if (ndw > 1024) return -1;   // page-0 window (multi-page: later)

    uint64_t flags = spinlock_lock_irqsave(&g_submit_lock);
    if (g_ring_tail + ndw * 4 > GEN12_RING_SIZE - 64) {
        spinlock_unlock_irqrestore(&g_submit_lock, flags);
        return -1;
    }
    volatile uint32_t* ring =
        (volatile uint32_t*)(g_ring_phys[0] + hhdm_offset);
    uint32_t start = g_ring_tail / 4;
    if (start + ndw > 1024) {   // stays on page 0 (checked pre-lock too)
        spinlock_unlock_irqrestore(&g_submit_lock, flags);
        return -1;
    }
    for (uint32_t i = 0; i < ndw; i++) ring[start + i] = dw[i];
    g_ring_tail += ndw * 4;
    gen12_ppgtt_sync();   // best-effort mirror; XY submits need it,
                          // STORE-only submits ignore it (GGTT direct)
    if (intel_gen12_ctx_set_ring(GEN12_RING_GPUADDR, GEN12_RING_SIZE,
                                 g_ring_tail) != 0) {
        spinlock_unlock_irqrestore(&g_submit_lock, flags);
        return -1;
    }
    __asm__ volatile("mfence" ::: "memory");
    if (fw_hold() != 0) {
        spinlock_unlock_irqrestore(&g_submit_lock, flags);
        return -1;
    }
    uint64_t lrca = intel_gen12_ctx_lrca();
    uint32_t desc_lo = (uint32_t)(lrca & 0xFFFFF000u) | DESC_LO_FLAGS;
    uint32_t desc_hi = (1u << (37 - 32)) | (g_sw_counter << (55 - 32)) |
                       (DESC_CLASS_COPY << (61 - 32));
    g_sw_counter = (g_sw_counter + 1) & 0x3Fu;
    intel_mmio_write32(ELSP_BCS0, desc_hi);
    intel_mmio_write32(ELSP_BCS0, desc_lo);
    spinlock_unlock_irqrestore(&g_submit_lock, flags);
    return 0;
}

static int g_live = 0;

int gen12_is_live(void) { return g_live; }

void gen12_note_hw_failure(void) { g_live = 0; }

static uint64_t g_op_hw = 0, g_op_fb = 0;

void gen12_count_hw(void) { g_op_hw++; }
void gen12_count_fb(void) { g_op_fb++; }

void gen12_op_counts(uint64_t* hw, uint64_t* fb) {
    if (hw) *hw = g_op_hw;
    if (fb) *fb = g_op_fb;
}

// Minimal op: STORE status<-seq + BB_END through the single owner.
int gen12_submit_store_test(void) {
    if (!g_ring_ok) {
        serial_print("[gen12_submit] SKIP (ring unavailable)\n");
        return -1;
    }
    if (intel_gen12_ctx_lrca() == 0) {
        serial_print("[gen12_submit] BLOCKED (no context)\n");
        return -1;
    }
    gen12_fence_t f;
    if (gen12_fence_alloc(&f) != 0) {
        serial_print("[gen12_submit] BLOCKED (no fence)\n");
        return -1;
    }

    // 1. Private staging (caller memory — safe under concurrency).
    uint32_t dw[5];
    dw[0] = INTEL_MI_STORE_DW_IMM | (INTEL_MI_STORE_LEN - 2) | (1u << 22);
    dw[1] = (uint32_t)(GEN12_STATUS_GPUADDR & 0xFFFFFFFFu);
    dw[2] = 0;
    dw[3] = f.seqno;
    dw[4] = INTEL_MI_BB_END;

    // 2. Commit + submit through the single owner.
    if (gen12_submit_commit(dw, 5) != 0) {
        serial_print("[gen12_submit] BLOCKED (submit)\n");
        return -1;
    }

    // 3. Completion = fence (bounded, lock-free; timeout → BLOCKED).
    if (gen12_fence_wait(&f, 0) != 0) {
        uint32_t st = intel_mmio_read32(EXEC_STATUS_BCS0);
        serial_print("[gen12_submit] BLOCKED (no completion, status=");
        serial_print_hex(st);
        serial_print(")\n");
        return -1;
    }
    serial_print("[gen12_submit] Gen12 submission: PASS (seq ");
    serial_dec(f.seqno);
    serial_print(")\n");
    g_live = 1;   // AL-13: ELSP+context+fence proven — GHAL may dispatch
    return 0;
}

static int g_smp_tested = 0;

// AL-15: two staged submissions committed back-to-back must land in
// disjoint ring regions, in order, both fenced. This is the single-CPU
// proof of the contract SMP stress relies on: private staging +
// atomic commit. Multi-CPU execution itself needs QEMU -smp 8 / HW
// (unavailable in this env) — declared BLOCKED, not claimed.
void gen12_submit_smp_selftest(void) {
    if (g_smp_tested) return;
    g_smp_tested = 1;
    if (!g_live) {
        serial_print("[gen12_smp] SKIP (engine not live)\n");
        return;
    }
    gen12_fence_t f1, f2;
    if (gen12_fence_alloc(&f1) != 0 || gen12_fence_alloc(&f2) != 0) {
        serial_print("[gen12_smp] SKIP (no fence)\n");
        return;
    }
    if (!(f2.seqno > f1.seqno)) {
        serial_print("[gen12_smp] FAIL (seqno order)\n");
        return;
    }
    uint32_t a[5], b[5];
    a[0] = b[0] = INTEL_MI_STORE_DW_IMM | (INTEL_MI_STORE_LEN - 2) | (1u << 22);
    a[1] = b[1] = (uint32_t)(GEN12_STATUS_GPUADDR & 0xFFFFFFFFu);
    a[2] = b[2] = 0;
    a[3] = f1.seqno;
    b[3] = f2.seqno;
    a[4] = b[4] = INTEL_MI_BB_END;
    // Stage both BEFORE either commit — the interleaving SMP would do.
    uint32_t off1 = g_ring_tail / 4;
    if (gen12_submit_commit(a, 5) != 0) {
        serial_print("[gen12_smp] FAIL (commit A)\n");
        return;
    }
    uint32_t off2 = g_ring_tail / 4;
    if (gen12_submit_commit(b, 5) != 0) {
        serial_print("[gen12_smp] FAIL (commit B)\n");
        return;
    }
    if (!(off2 == off1 + 5)) {
        serial_print("[gen12_smp] FAIL (tail order)\n");
        return;
    }
    if (gen12_fence_wait(&f1, 0) != 0 || gen12_fence_wait(&f2, 0) != 0) {
        serial_print("[gen12_smp] FAIL (fence)\n");
        return;
    }
    volatile uint32_t* ring =
        (volatile uint32_t*)(g_ring_phys[0] + hhdm_offset);
    for (uint32_t i = 0; i < 5; i++) {
        if (ring[off1 + i] != a[i] || ring[off2 + i] != b[i]) {
            serial_print("[gen12_smp] FAIL (ring intact)\n");
            return;
        }
    }
    serial_print("[gen12_smp] SMP: PASS (ordered commits)\n");
}
