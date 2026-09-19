// ============================================================
// Gen12 GPU virtual memory — AL-4
// (graphics/backend/intel_gen12_vm.c)
//
// DETERMINATION (from i915, drivers/gpu/drm/i915/gt/intel_lrc.c):
// execlist context state, ringbuffer, HWSP and LRCA/descriptor are
// all pinned and addressed in GGTT (i915_ggtt_offset / PIN_GLOBAL).
// Per-context PPGTT roots exist (init_ppgtt_regs) but the MINIMUM
// for one serialized 2D owner is GGTT-only: context + ring +
// status + batch/data buffers all GGTT-mapped, no PPGTT tables,
// no per-context VM. Whether the CS accepts GGTT batch addresses
// is settled by context init (AL-5); this phase only guarantees
// every Gen12 GPU address we emit lies inside the GGTT window.
// Overlap safety for Gen12 fixed regions is reservation-by-
// construction in AL-5 — legacy intel_gtt_map_pages() is frozen
// and untouched by this phase.
//
// Ownership: PMM owns pages; GTT owns PTE mappings (cleared on
// unmap). CPU view = phys + HHDM; GPU view = GGTT vaddr. The two
// never mix: validators reject CPU pointers as GPU addresses.
// ============================================================

#include "intel_gen12.h"
#include "intel_regs.h"
#include "intel_gtt.h"
#include "pmm.h"

extern intel_gen_t intel_get_generation(void);
extern void serial_print(const char* s);

#define GEN12_GGTT_WINDOW ((uint64_t)INTEL_GTT_MAX_ENTRIES * 4096ULL)

// Pure address check: inside GGTT window, 4K-aligned, non-empty, no wrap.
// No HW touch — always testable.
int intel_gen12_vm_valid(uint64_t gpu_addr, uint64_t len) {
    if (len == 0) return -1;
    if (gpu_addr & 0xFFFULL) return -1;
    if (len & 0xFFFULL) return -1;
    if (gpu_addr + len < gpu_addr) return -1;   // wrap
    if (gpu_addr + len > GEN12_GGTT_WINDOW) return -1;
    return 0;
}

static int g_vm_tested = 0;

void intel_gen12_vm_selftest(void) {
    if (g_vm_tested) return;
    g_vm_tested = 1;

    // Pure validator checks (no GTT needed).
    if (intel_gen12_vm_valid(0x1000, 0x1000) != 0) goto fail;
    if (intel_gen12_vm_valid(0x1001, 0x1000) == 0) goto fail;  // unaligned
    if (intel_gen12_vm_valid(0x1000, 0) == 0) goto fail;       // empty
    if (intel_gen12_vm_valid(GEN12_GGTT_WINDOW - 0x1000, 0x1000) != 0) goto fail;
    if (intel_gen12_vm_valid(GEN12_GGTT_WINDOW - 0x1000, 0x2000) == 0) goto fail;
    if (intel_gen12_vm_valid(~0ULL - 0xFFFULL, 0x2000) == 0) goto fail;  // wrap

    // Live checks (need programmed GTT; SKIP without one).
    // Uses ONLY bump + unmap + fixed-remap-after-free: valid under
    // frozen legacy semantics. Overlap protection for Gen12 fixed
    // regions comes from reservation-by-construction in AL-5, never
    // by changing intel_gtt_map_pages().
    if (!intel_gtt_is_active()) {
        serial_print("[gen12_vm] SKIP (GTT unavailable)\n");
        return;
    }
    phys_addr_t pg = pmm_alloc_page();
    if (pg == PHYS_NULL) {
        serial_print("[gen12_vm] SKIP (no PMM page)\n");
        return;
    }
    uint32_t idx = intel_gtt_map_pages(0, &pg, 1);   // bump: free region
    if (idx == (uint32_t)-1) { pmm_free_page(pg); goto fail; }
    uint64_t vaddr = (uint64_t)idx * 4096ULL;
    if (intel_gen12_vm_valid(vaddr, 4096) != 0) {     // bump must be in-window
        intel_gtt_unmap(idx, 1); pmm_free_page(pg); goto fail;
    }
    intel_gtt_unmap(idx, 1);
    if (intel_gtt_map_pages(vaddr, &pg, 1) == (uint32_t)-1) {  // freed: accept
        pmm_free_page(pg); goto fail;
    }
    intel_gtt_unmap(idx, 1);
    pmm_free_page(pg);

    serial_print("[gen12_vm] VM: PASS (GGTT window)\n");
    return;
fail:
    serial_print("[gen12_vm] VM: FAIL\n");
}
