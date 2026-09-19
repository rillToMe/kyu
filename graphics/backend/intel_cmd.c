// ============================================================
// Intel GPU Command Buffer — Phase 8
// (graphics/backend/intel_cmd.c)
//
// Staging in CPU RAM, submit via intel_bcs_submit().
// MI encodings are generation-stable. XY COPY/FILL layout
// follows i915 (XY_SRC_COPY_BLT / XY_COLOR_BLT) — field order
// documented per DWORD, HW-validated in Phase 9/10.
// Clipping disabled in builders (kernel rect checks own it,
// full validation lands in Phase 12).
// ============================================================

#include "intel_cmd.h"
#include "intel_bcs.h"
#include "intel_regs.h"
#include "intel_gtt.h"
#include "pmm.h"
#include <string.h>

extern uint64_t hhdm_offset;
extern void serial_print(const char* s);

// --- Status page (1 PMM page, GPU-writable via GTT) ---
static uint32_t* g_status_cpu;
static phys_addr_t g_status_phys;
static int g_status_ok = 0;

int intel_cmd_status_init(void) {
    if (g_status_ok) return 0;
    g_status_phys = pmm_alloc_page();
    if (g_status_phys == PHYS_NULL) return -1;
    g_status_cpu = (uint32_t*)(g_status_phys + hhdm_offset);
    memset(g_status_cpu, 0, 4096);
    if (intel_gtt_map_pages(INTEL_BCS_STATUS_GPUADDR, &g_status_phys, 1)
        == (uint32_t)-1) {
        pmm_free_page(g_status_phys);
        return -1;
    }
    g_status_ok = 1;
    return 0;
}

uint64_t intel_cmd_status_gpu_addr(void) { return INTEL_BCS_STATUS_GPUADDR; }
uint32_t intel_cmd_status_read(void) { return g_status_ok ? *g_status_cpu : 0; }
void intel_cmd_status_write(uint32_t v) { if (g_status_ok) *g_status_cpu = v; }

// --- Staging ---

void intel_cmd_begin(intel_cmd_t* c) {
    if (c) c->n = 0;
}

static int push(intel_cmd_t* c, uint32_t v) {
    if (!c || c->n >= INTEL_CMD_MAX_DWORDS) return -1;
    c->dw[c->n++] = v;
    return 0;
}

int intel_cmd_emit_noop(intel_cmd_t* c) { return push(c, INTEL_MI_NOOP); }

int intel_cmd_emit_bb_end(intel_cmd_t* c) { return push(c, INTEL_MI_BB_END); }

int intel_cmd_emit_store(intel_cmd_t* c, uint64_t gpu_addr, uint32_t value) {
    if (!c) return -1;
    // MI_STORE_DWORD_IMM, len 4: hdr, addr_lo, addr_hi, data.
    if (push(c, INTEL_MI_STORE_DW_IMM | (INTEL_MI_STORE_LEN - 2))) return -1;
    if (push(c, (uint32_t)(gpu_addr & 0xFFFFFFFF))) return -1;
    if (push(c, (uint32_t)(gpu_addr >> 32))) return -1;
    return push(c, value);
}

int intel_cmd_emit_copy(intel_cmd_t* c, uint64_t dst_gpu, uint32_t dst_pitch,
                        uint64_t src_gpu, uint32_t src_pitch,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!c || w == 0 || h == 0) return -1;
    // XY_SRC_COPY_BLT, 12 DWORDs (UNVALIDATED — Phase 9 proves it):
    // dw0 hdr | dw1 BR13(rop+depth) | dw2 dst_pitch | dw3/4 dst_addr
    // dw5 src_pitch | dw6/7 src_addr | dw8 dst_xy | dw9 src_xy
    // dw10 size_wh | dw11 clip/disabled=0
    uint32_t br13 = (INTEL_ROP_SRCCOPY << 16) | (INTEL_BLT_DEPTH_32BPP << 24);
    if (push(c, INTEL_XY_SRC_COPY | (INTEL_XY_COPY_LEN - 2))) return -1;
    if (push(c, br13)) return -1;
    if (push(c, dst_pitch * 4)) return -1;
    if (push(c, (uint32_t)(dst_gpu & 0xFFFFFFFF))) return -1;
    if (push(c, (uint32_t)(dst_gpu >> 32))) return -1;
    if (push(c, src_pitch * 4)) return -1;
    if (push(c, (uint32_t)(src_gpu & 0xFFFFFFFF))) return -1;
    if (push(c, (uint32_t)(src_gpu >> 32))) return -1;
    if (push(c, (y << 16) | (x & 0xFFFF))) return -1;
    if (push(c, (y << 16) | (x & 0xFFFF))) return -1;
    if (push(c, (h << 16) | (w & 0xFFFF))) return -1;
    return push(c, 0);
}

int intel_cmd_emit_blit(intel_cmd_t* c, uint64_t dst_gpu, uint32_t dst_pitch,
                        uint64_t src_gpu, uint32_t src_pitch,
                        uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                        uint32_t w, uint32_t h) {
    if (!c || w == 0 || h == 0) return -1;
    // Same 12-DWORD XY layout as COPY; dw8 = dst_xy, dw9 = src_xy
    // differ here (UNVALIDATED — Phase 11 proves it).
    uint32_t br13 = (INTEL_ROP_SRCCOPY << 16) | (INTEL_BLT_DEPTH_32BPP << 24);
    if (push(c, INTEL_XY_SRC_COPY | (INTEL_XY_COPY_LEN - 2))) return -1;
    if (push(c, br13)) return -1;
    if (push(c, dst_pitch * 4)) return -1;
    if (push(c, (uint32_t)(dst_gpu & 0xFFFFFFFF))) return -1;
    if (push(c, (uint32_t)(dst_gpu >> 32))) return -1;
    if (push(c, src_pitch * 4)) return -1;
    if (push(c, (uint32_t)(src_gpu & 0xFFFFFFFF))) return -1;
    if (push(c, (uint32_t)(src_gpu >> 32))) return -1;
    if (push(c, (dy << 16) | (dx & 0xFFFF))) return -1;
    if (push(c, (sy << 16) | (sx & 0xFFFF))) return -1;
    if (push(c, (h << 16) | (w & 0xFFFF))) return -1;
    return push(c, 0);
}

int intel_cmd_emit_fill(intel_cmd_t* c, uint64_t dst_gpu, uint32_t dst_pitch,
                        uint32_t color,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!c || w == 0 || h == 0) return -1;
    // XY_COLOR_BLT, 8 DWORDs (UNVALIDATED — Phase 10 proves it):
    // dw0 hdr | dw1 BR13(pat-rop+depth) | dw2 pitch | dw3/4 addr
    // dw5 color | dw6 dst_xy | dw7 size_wh
    uint32_t br13 = (INTEL_ROP_PATCOPY << 16) | (INTEL_BLT_DEPTH_32BPP << 24);
    if (push(c, INTEL_XY_COLOR_BLT | (INTEL_XY_FILL_LEN - 2))) return -1;
    if (push(c, br13)) return -1;
    if (push(c, dst_pitch * 4)) return -1;
    if (push(c, (uint32_t)(dst_gpu & 0xFFFFFFFF))) return -1;
    if (push(c, (uint32_t)(dst_gpu >> 32))) return -1;
    if (push(c, color)) return -1;
    if (push(c, (y << 16) | (x & 0xFFFF))) return -1;
    return push(c, (h << 16) | (w & 0xFFFF));
}

int intel_cmd_submit(intel_cmd_t* c) {
    if (!c || c->n == 0) return -1;
    return intel_bcs_submit(c->dw, c->n);
}

int intel_cmd_wait(uint32_t max_spins) {
    return intel_bcs_wait_idle(max_spins);
}

// --- Builder-only self-test (no HW touch) ---

int intel_cmd_selftest(void) {
    intel_cmd_t c;
    intel_cmd_begin(&c);
    if (intel_cmd_emit_noop(&c)) goto fail;
    if (intel_cmd_emit_noop(&c)) goto fail;
    if (intel_cmd_emit_store(&c, 0x10000ULL, 0x12345678)) goto fail;
    if (intel_cmd_emit_bb_end(&c)) goto fail;
    // Expect 2 + 4 + 1 = 7 DWORDs, magic headers intact.
    if (c.n != 7) goto fail;
    if (c.dw[0] != INTEL_MI_NOOP) goto fail;
    if (c.dw[2] != (INTEL_MI_STORE_DW_IMM | (INTEL_MI_STORE_LEN - 2))) goto fail;
    if (c.dw[6] != INTEL_MI_BB_END) goto fail;
    if (c.dw[3] != 0x10000 || c.dw[5] != 0x12345678) goto fail;

    intel_cmd_t cp;
    intel_cmd_begin(&cp);
    if (intel_cmd_emit_copy(&cp, 0x100000ULL, 64, 0x200000ULL, 64, 0, 0, 8, 8)) goto fail;
    if (cp.n != INTEL_XY_COPY_LEN) goto fail;
    if ((cp.dw[0] & 0xFF800000u) != (INTEL_XY_SRC_COPY & 0xFF800000u)) goto fail;

    intel_cmd_t fl;
    intel_cmd_begin(&fl);
    if (intel_cmd_emit_fill(&fl, 0x100000ULL, 64, 0xFF00FF00, 0, 0, 8, 8)) goto fail;
    if (fl.n != INTEL_XY_FILL_LEN) goto fail;
    if ((fl.dw[0] & 0xFF800000u) != (INTEL_XY_COLOR_BLT & 0xFF800000u)) goto fail;
    if (fl.dw[5] != 0xFF00FF00) goto fail;

    serial_print("[intel_cmd] selftest PASS\n");
    return 0;
fail:
    serial_print("[intel_cmd] selftest FAIL\n");
    return -1;
}
