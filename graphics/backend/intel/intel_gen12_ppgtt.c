// ============================================================
// Gen12 minimal PPGTT — AL-9
// (graphics/backend/intel/intel_gen12_ppgtt.c)
//
// ENCODING SOURCES (i915, torvalds/linux):
// - Leaf: gen12_pte_encode() (gen8_ppgtt.c) for sysmem pat 0:
//   addr | PRESENT | RW  (PAT0-2 clear = PAT index 0).
// - Non-leaf: gen8_pde_encode(): addr | PRESENT | RW | PPAT_*;
//   PPAT_CACHED_PDE = 0 (intel_gtt.h), so PD[0] = PT_phys | 0x3.
// - PRESENT = BIT(0), RW = BIT(1) (GEN8_PAGE_*, intel_gtt.h).
// - 3-level split 30|21|12 (intel_gtt.h comment): our window < 2MB
//   → PDP[0] → PD[0] → PT[i]. PDP slots take *physical* addresses
//   (ASSIGN_CTX_PDP uses i915_page_dir_dma_addr).
// - CPU/table coherency: i915 clflushes table writes
//   (write_dma_entry); mirrored here.
//
// RESIDUAL RISKS (not guessed away, observed on HW run):
// - PAT index 0 without tgl_setup_private_ppat programming: HW-reset
//   default assumed WB; if UC, still correct, only slower.
// - Data-TLB staleness across submits: mitigated by FORCE_RESTORE
//   on every submit (full restore re-fetches PDPs); a mismatch on
//   the HW run points here first.
// ============================================================

#include "intel_gen12_ppgtt.h"
#include "intel_gen12.h"
#include "intel_gen12_ctx.h"
#include "intel_regs.h"
#include "intel_gtt.h"
#include "pmm.h"
#include <string.h>

extern intel_gen_t intel_get_generation(void);
extern void serial_print(const char* s);
extern uint64_t hhdm_offset;

#define PTE_PRESENT  (1ULL << 0)
#define PTE_RW       (1ULL << 1)
#define PTE_LEAF     (PTE_PRESENT | PTE_RW)   // sysmem, PAT index 0
#define PDE_TOPD     (PTE_PRESENT | PTE_RW)   // PPAT_CACHED_PDE = 0

static int      g_pp_ok = 0;
static uint64_t g_pd_phys = 0;
static uint64_t g_pt_phys = 0;

void gen12_cpu_clflush(uint64_t phys, uint64_t len) {
    if (len == 0) return;
    uint64_t start = (phys + hhdm_offset) & ~63ULL;
    uint64_t end = phys + hhdm_offset + len;
    for (uint64_t p = start; p < end; p += 64)
        __asm__ volatile("clflush %0" : "+m" (*(volatile char*)p));
    __asm__ volatile("mfence" ::: "memory");
}

int gen12_ppgtt_init(void) {
    if (g_pp_ok) return 0;
    if (intel_get_generation() != INTEL_GEN12) {
        serial_print("[gen12_ppgtt] SKIP (not Gen12)\n");
        return -1;
    }
    if (intel_gen12_ctx_lrca() == 0) {
        serial_print("[gen12_ppgtt] BLOCKED (no context)\n");
        return -1;
    }
    g_pd_phys = pmm_alloc_page();
    g_pt_phys = pmm_alloc_page();
    if (g_pd_phys == PHYS_NULL || g_pt_phys == PHYS_NULL) {
        if (g_pd_phys != PHYS_NULL) pmm_free_page(g_pd_phys);
        if (g_pt_phys != PHYS_NULL) pmm_free_page(g_pt_phys);
        g_pd_phys = g_pt_phys = 0;
        serial_print("[gen12_ppgtt] SKIP (no PMM pages)\n");
        return -1;
    }
    memset((void*)(g_pd_phys + hhdm_offset), 0, 4096);
    memset((void*)(g_pt_phys + hhdm_offset), 0, 4096);
    // PD[0] → PT (rest zero = never walked below 1GB... PD[1..511]
    // cover ≥2MB, outside our window).
    ((volatile uint64_t*)(g_pd_phys + hhdm_offset))[0] =
        (g_pt_phys & ~0xFFFULL) | PDE_TOPD;
    gen12_cpu_clflush(g_pd_phys, 4096);
    if (intel_gen12_ctx_set_pdp(g_pd_phys) != 0) {
        pmm_free_page(g_pd_phys);
        pmm_free_page(g_pt_phys);
        g_pd_phys = g_pt_phys = 0;
        serial_print("[gen12_ppgtt] FAIL (PDP patch)\n");
        return -1;
    }
    if (gen12_ppgtt_sync() != 0) {
        serial_print("[gen12_ppgtt] FAIL (initial sync)\n");
        return -1;
    }
    g_pp_ok = 1;
    serial_print("[gen12_ppgtt] PPGTT ready (PD mirrors GGTT)\n");
    return 0;
}

// Mirror GGTT PTEs into the PT. Same phys, leaf encoding.
// NOTE: read-only walk of GGTT under no lock by design here — all
// maps in the AL-9 path happen-before the submit on one CPU;
// SMP hardening is AL-15's phase (roadmap), not this one.
int gen12_ppgtt_sync(void) {
    if (g_pd_phys == 0 || g_pt_phys == 0) return -1;
    volatile uint64_t* pt = (volatile uint64_t*)(g_pt_phys + hhdm_offset);
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t gpte = intel_gtt_pte_raw(i);
        if (gpte & PTE_PRESENT)
            pt[i] = (gpte & ~0xFFFULL) | PTE_LEAF;
        else
            pt[i] = 0;
    }
    gen12_cpu_clflush(g_pt_phys, 4096);
    return 0;
}
