// ============================================================
// Gen12 BCS0 Logical Ring Context — AL-5
// (graphics/backend/intel/intel_gen12_ctx.c)
//
// SOURCES (i915, torvalds/linux, no guessing):
// - Layout: __lrc_alloc_state() — context_size pages + 2 WA pages
//   on Gen12. COPY class size: intel_engine_context_size() →
//   GEN8_LR_CONTEXT_OTHER_SIZE = 2 pages (all gens ≥8). Total: 4.
// - Image: gen12_xcs_offsets[] + set_offsets() (intel_lrc.c).
// - CTX_* indices: intel_lrc_reg.h (HEAD 5, TAIL 7, START 9,
//   CTL 11, CONTROL 3, TIMESTAMP 35).
// - CTX_CONTROL value: init_common_regs(), gen ≥ 11, inhibit:
//   ENABLE(INHIBIT_SYN_CTX_SWITCH=BIT3) | DISABLE(RESTORE_INHIBIT)
//   | RESTORE_INHIBIT = 0x80008 | 0x10000 | 0x1 = 0x90009.
// - LRI header: MI_LOAD_REGISTER_IMM(n)=(0x22<<23)|(2n-1) +
//   POSTED(1<<12) + LRM_CS_MMIO(BIT19) for Gen12 (intel_gpu_commands.h,
//   set_offsets adds LRM_CS_MMIO for gen ≥ 11).
// - BCS mmio_base 0x12000: engine table BCS0 → BLT_RING_BASE
//   (intel_engine_cs.c; COPY class uses absolute LRI, cf.
//   i915_engine_has_relative_lri() == false for COPY).
// - MI_MODE slot 0x60, BB_OFFSET 0x70 zeroed (lrc_ring_mi_mode /
//   lrc_ring_bb_offset return 0x60/0x70 on Gen12, init_common_regs
//   writes regs[loc+1] = 0).
// - Pages need not be contiguous: GGTT maps per-page phys.
// - PDP slots stay 0 (no PPGTT — AL-4 determination); RING_* slots
//   stay 0 here, patched at submit (AL-8, cf. lrc_update_regs).
// ============================================================

#include "intel_gen12_ctx.h"
#include "intel_gen12.h"
#include "intel_regs.h"
#include "intel_gtt.h"
#include "pmm.h"
#include <string.h>

extern intel_gen_t intel_get_generation(void);
extern void serial_print(const char* s);
extern void serial_print_hex(uint64_t v);

#define BCS_MMIO_BASE  0x12000u

// LRI headers for the two gen12_xcs blocks.
#define LRI_HDR(n)     ((0x22u << 23) | (2u * (n) - 1u) | (1u << 12) | (1u << 19))
#define LRI_HDR_13     LRI_HDR(13)   // = 0x1181019
#define LRI_HDR_9      LRI_HDR(9)    // = 0x1181011

// CTX image slots (intel_lrc_reg.h).
#define CTX_CONTROL    3
#define CTX_RING_HEAD  5
#define CTX_RING_TAIL  7
#define CTX_RING_START 9
#define CTX_RING_CTL   11
#define CTX_TIMESTAMP  35
#define CTX_MI_MODE    0x60
#define CTX_BB_OFF     0x70

#define CTX_CONTROL_VAL 0x90009u

// gen12_xcs_offsets[] register lists (relative to BCS_MMIO_BASE).
static const uint16_t xcs_blk1[13] = {
    0x244, 0x034, 0x030, 0x038, 0x03c, 0x168, 0x140,
    0x110, 0x1c0, 0x1c4, 0x1c8, 0x180, 0x2b4
};
static const uint16_t xcs_blk2[9] = {
    0x3a8, 0x28c, 0x288, 0x284, 0x280, 0x27c, 0x278, 0x274, 0x270
};

static int      g_ctx_active = 0;
static uint64_t g_ctx_phys[GEN12_CTX_PAGES];
static uint32_t* g_ctx_cpu = 0;   // HHDM, page 0 (PPHWSP); +4096 = state

uint64_t intel_gen12_ctx_lrca(void) {
    return g_ctx_active ? GEN12_CTX_GPUADDR : 0;
}

// Build the state page (page 1). Mirrors set_offsets() +
// init_common_regs() for the Gen12-xcs case, values zero except
// CTX_CONTROL (the LRI pairs program the MMIO addrs on restore).
static void ctx_build_image(uint32_t* st) {
    for (uint32_t i = 0; i < 1024; i++) st[i] = 0;
    uint32_t p = 1;                       // NOP(1)
    st[p++] = LRI_HDR_13;                 // LRI(13, POSTED)
    for (uint32_t k = 0; k < 13; k++) {
        st[p++] = BCS_MMIO_BASE + xcs_blk1[k];
        st[p++] = 0;
    }
    p += 5;                               // NOP(5)
    st[p++] = LRI_HDR_9;                  // LRI(9, POSTED)
    for (uint32_t k = 0; k < 9; k++) {
        st[p++] = BCS_MMIO_BASE + xcs_blk2[k];
        st[p++] = 0;
    }
    st[CTX_CONTROL] = CTX_CONTROL_VAL;    // overwrites pair-0 value slot
    st[CTX_TIMESTAMP] = 0;
    st[CTX_MI_MODE + 1] = 0;
    st[CTX_BB_OFF + 1] = 0;
}

int intel_gen12_ctx_validate(const uint32_t* st) {
    if (!st) return -1;
    if (st[0] != 0) return -1;
    if (st[1] != LRI_HDR_13) return -1;
    if (st[2] != BCS_MMIO_BASE + 0x244) return -1;   // CONTEXT_CONTROL addr
    if (st[4] != BCS_MMIO_BASE + 0x034) return -1;   // HEAD addr
    if (st[3] != CTX_CONTROL_VAL) return -1;
    if (st[5] != 0 || st[7] != 0 || st[9] != 0 || st[11] != 0) return -1;
    if (st[33] != LRI_HDR_9) return -1;
    if (st[34] != BCS_MMIO_BASE + 0x3a8) return -1;  // TIMESTAMP addr
    if (st[35] != 0) return -1;
    if (st[CTX_MI_MODE + 1] != 0) return -1;
    if (st[CTX_BB_OFF + 1] != 0) return -1;
    return 0;
}

int intel_gen12_ctx_create(void) {
    if (g_ctx_active) return 0;
    if (intel_get_generation() != INTEL_GEN12) {
        serial_print("[gen12_ctx] SKIP (not Gen12)\n");
        return -1;
    }
    if (!g_gen12_engines.copy_present) {
        serial_print("[gen12_ctx] BLOCKED (no copy engine)\n");
        return -1;
    }
    if (!intel_gtt_is_active()) {
        serial_print("[gen12_ctx] SKIP (GTT unavailable)\n");
        return -1;
    }
    for (uint32_t i = 0; i < GEN12_CTX_PAGES; i++) {
        g_ctx_phys[i] = pmm_alloc_page();
        if (g_ctx_phys[i] == PHYS_NULL) {
            for (uint32_t j = 0; j < i; j++) pmm_free_page(g_ctx_phys[j]);
            serial_print("[gen12_ctx] SKIP (no PMM pages)\n");
            return -1;
        }
    }
    if (intel_gen12_vm_valid(GEN12_CTX_GPUADDR,
                             GEN12_CTX_PAGES * 4096ULL) != 0) {
        for (uint32_t j = 0; j < GEN12_CTX_PAGES; j++)
            pmm_free_page(g_ctx_phys[j]);
        serial_print("[gen12_ctx] FAIL (reservation outside GGTT)\n");
        return -1;
    }
    if (intel_gtt_map_pages(GEN12_CTX_GPUADDR, g_ctx_phys,
                            GEN12_CTX_PAGES) == (uint32_t)-1) {
        for (uint32_t j = 0; j < GEN12_CTX_PAGES; j++)
            pmm_free_page(g_ctx_phys[j]);
        serial_print("[gen12_ctx] FAIL (GGTT map)\n");
        return -1;
    }
    extern uint64_t hhdm_offset;
    // PMM pages are not zeroed — clear each via its own HHDM address
    // (pages are NOT assumed contiguous).
    for (uint32_t j = 0; j < GEN12_CTX_PAGES; j++)
        memset((void*)(g_ctx_phys[j] + hhdm_offset), 0, 4096);
    g_ctx_cpu = (uint32_t*)(g_ctx_phys[0] + hhdm_offset);
    uint32_t* state = (uint32_t*)(g_ctx_phys[1] + hhdm_offset);
    ctx_build_image(state);   // page 1 = register state
    if (intel_gen12_ctx_validate(state) != 0) {
        intel_gtt_unmap((uint32_t)(GEN12_CTX_GPUADDR >> 12), GEN12_CTX_PAGES);
        for (uint32_t j = 0; j < GEN12_CTX_PAGES; j++)
            pmm_free_page(g_ctx_phys[j]);
        g_ctx_cpu = 0;
        serial_print("[gen12_ctx] FAIL (image)\n");
        return -1;
    }
    g_ctx_active = 1;
    serial_print("[gen12_ctx] context ready (LRCA ");
    serial_print_hex(GEN12_CTX_GPUADDR);
    serial_print(")\n");
    return 0;
}

void intel_gen12_ctx_destroy(void) {
    if (!g_ctx_active) return;
    intel_gtt_unmap((uint32_t)(GEN12_CTX_GPUADDR >> 12), GEN12_CTX_PAGES);
    for (uint32_t j = 0; j < GEN12_CTX_PAGES; j++)
        pmm_free_page(g_ctx_phys[j]);
    g_ctx_cpu = 0;
    g_ctx_active = 0;
}

// AL-8: patch ring registers into the state image before submit.
// CTL = (size - 4K) | VALID (RING_CTL_SIZE + RING_VALID, intel_engine_regs.h).
int intel_gen12_ctx_set_ring(uint64_t ring_gpu, uint32_t ring_size,
                             uint32_t tail_bytes) {    if (!g_ctx_active) return -1;
    if ((ring_gpu & 0xFFFULL) || ring_size < 8192u) return -1;
    if ((tail_bytes & 7u) || tail_bytes >= ring_size) return -1;
    if (intel_gen12_vm_valid(ring_gpu, ring_size) != 0) return -1;
    extern uint64_t hhdm_offset;
    uint32_t* state = (uint32_t*)(g_ctx_phys[1] + hhdm_offset);
    state[CTX_RING_START] = (uint32_t)(ring_gpu & 0xFFFFFFFFu);
    state[CTX_RING_HEAD] = 0;
    state[CTX_RING_TAIL] = tail_bytes;
    state[CTX_RING_CTL] = (ring_size - 4096u) | 1u;
    return 0;
}

// CTX PDP slots (intel_lrc_reg.h): PDP0_UDW=49, PDP0_LDW=51
// (PDP1-3 stay 0 — never walked for addresses < 1GB).
#define CTX_PDP0_UDW 49
#define CTX_PDP0_LDW 51

// AL-9: program the PPGTT root. pd_phys is the physical address of our
// PD page (ASSIGN_CTX_PDP uses physical i915_page_dir_dma_addr).
int intel_gen12_ctx_set_pdp(uint64_t pd_phys) {
    if (!g_ctx_active) return -1;
    if ((pd_phys & 0xFFFULL) || pd_phys == 0) return -1;
    extern uint64_t hhdm_offset;
    uint32_t* state = (uint32_t*)(g_ctx_phys[1] + hhdm_offset);
    state[CTX_PDP0_UDW] = (uint32_t)(pd_phys >> 32);
    state[CTX_PDP0_LDW] = (uint32_t)(pd_phys & 0xFFFFFFFFu);
    return 0;
}
