// ============================================================
// Intel GPU COPY test — Phase 9
// (graphics/backend/intel/intel_test_copy.c)
//
// Full 64x64 copy, src pattern = index pixels, dst zeroed.
// CPU access is per-page via phys_addrs (safe when PMM pages
// are not contiguous). iGPU snoops LLC so no explicit cache
// flush between CPU fill and GPU read.
// ============================================================

#include "intel_test_copy.h"
#include "intel_gpu_alloc.h"
#include "intel_rect.h"
#include "intel_regs.h"
#include "intel_bcs.h"
#include "intel_cmd.h"
#include "intel_fence.h"

extern uint64_t hhdm_offset;
extern void serial_print(const char* s);

#define COPY_W 64
#define COPY_H 64

// Per-page pixel write (page = y*stride/1024th page math via byte offset).
static void buf_write(intel_gpu_buffer_t* b, uint32_t x, uint32_t y, uint32_t v) {
    uint64_t byte = ((uint64_t)y * b->stride + x) * 4;
    uint32_t page = (uint32_t)(byte >> 12);
    uint32_t off = (uint32_t)(byte & 0xFFF);
    if (page >= b->num_pages) return;
    *(uint32_t*)(b->phys_addrs[page] + hhdm_offset + off) = v;
}

static uint32_t buf_read(intel_gpu_buffer_t* b, uint32_t x, uint32_t y) {
    uint64_t byte = ((uint64_t)y * b->stride + x) * 4;
    uint32_t page = (uint32_t)(byte >> 12);
    uint32_t off = (uint32_t)(byte & 0xFFF);
    if (page >= b->num_pages) return 0xDEADDEAD;
    return *(uint32_t*)(b->phys_addrs[page] + hhdm_offset + off);
}

int intel_test_copy_run(void) {
    int src = intel_gpu_buffer_create(COPY_W, COPY_H);
    int dst = intel_gpu_buffer_create(COPY_W, COPY_H);
    if (src < 0 || dst < 0) {
        serial_print("[intel_copy] GPU COPY: FAIL (alloc)\n");
        if (src >= 0) intel_gpu_buffer_destroy(src);
        if (dst >= 0) intel_gpu_buffer_destroy(dst);
        return -1;
    }
    if (intel_gpu_buffer_map_gpu(src) != 0 ||
        intel_gpu_buffer_map_gpu(dst) != 0) {
        serial_print("[intel_copy] GPU COPY: FAIL (map)\n");
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        return -1;
    }

    intel_gpu_buffer_t* sb = intel_gpu_buffer_get(src);
    intel_gpu_buffer_t* db = intel_gpu_buffer_get(dst);

    // Known pattern in src, zeros in dst.
    for (uint32_t y = 0; y < COPY_H; y++)
        for (uint32_t x = 0; x < COPY_W; x++)
            buf_write(sb, x, y, 0xFF000000u | (y * COPY_W + x));
    for (uint32_t y = 0; y < COPY_H; y++)
        for (uint32_t x = 0; x < COPY_W; x++)
            buf_write(db, x, y, 0);

    if (!intel_bcs_is_available()) {
        // No ring (Gen12/QEMU): verify the would-be command only.
        intel_cmd_t c;
        intel_cmd_begin(&c);
        int ok = intel_cmd_emit_copy(&c, db->gpu_vaddr, db->stride,
                                     sb->gpu_vaddr, sb->stride,
                                     0, 0, COPY_W, COPY_H);
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        if (ok == 0 && c.n == INTEL_XY_COPY_LEN) {
            serial_print("[intel_copy] GPU COPY: SKIP (no BCS)\n");
            return 1;
        }
        serial_print("[intel_copy] GPU COPY: FAIL (builder)\n");
        return -1;
    }

    // HW path: single COPY + BB_END, submit, wait, validate.
    // Pair gated by Phase-12 validator first.
    intel_rect_t cs = {0, 0, COPY_W, COPY_H}, cd = {0, 0, COPY_W, COPY_H};
    if (intel_blit_validate(db, sb, &cs, &cd)) {
        serial_print("[intel_copy] GPU COPY: FAIL (validate)\n");
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        return -1;
    }
    intel_cmd_t c;
    intel_cmd_begin(&c);
    if (intel_cmd_emit_copy(&c, db->gpu_vaddr, db->stride,
                            sb->gpu_vaddr, sb->stride,
                            0, 0, COPY_W, COPY_H) != 0) {
        serial_print("[intel_copy] GPU COPY: FAIL (emit)\n");
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        return -1;
    }
    intel_fence_t f;
    if (intel_gpu_submit(&c, &f) != 0) {
        serial_print("[intel_copy] GPU COPY: FAIL (submit)\n");
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        return -1;
    }
    if (intel_fence_wait_sleep(&f, 0) != 0) {
        serial_print("[intel_copy] GPU COPY: FAIL (timeout)\n");
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        return -1;
    }

    int bad = 0;
    for (uint32_t y = 0; y < COPY_H && !bad; y++)
        for (uint32_t x = 0; x < COPY_W; x++) {
            uint32_t want = 0xFF000000u | (y * COPY_W + x);
            if (buf_read(db, x, y) != want) { bad = 1; break; }
        }

    intel_gpu_buffer_destroy(src);
    intel_gpu_buffer_destroy(dst);

    if (bad) {
        serial_print("[intel_copy] GPU COPY: FAIL (mismatch)\n");
        return -1;
    }
    serial_print("[intel_copy] GPU COPY: PASS\n");
    return 0;
}
