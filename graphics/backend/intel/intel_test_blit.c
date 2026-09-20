// ============================================================
// Intel GPU BLIT test — Phase 11
// (graphics/backend/intel/intel_test_blit.c)
//
// 1:1 only, no scaling. Src 64x64 index pattern, dst zeroed.
// Per-page CPU access. Rect bounds pre-checked on CPU
// (full Phase-12 validation comes later).
// ============================================================

#include "intel_test_blit.h"
#include "intel_gpu_alloc.h"
#include "intel_rect.h"
#include "intel_regs.h"
#include "intel_bcs.h"
#include "intel_cmd.h"
#include "intel_fence.h"

extern uint64_t hhdm_offset;
extern void serial_print(const char* s);

#define BLIT_W 64
#define BLIT_H 64

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

static int hw_blit(intel_gpu_buffer_t* dst, intel_gpu_buffer_t* src,
                   uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                   uint32_t w, uint32_t h) {
    intel_rect_t s = {sx, sy, w, h}, d = {dx, dy, w, h};
    if (intel_blit_validate(dst, src, &s, &d)) return -1;
    intel_cmd_t c;
    intel_cmd_begin(&c);
    if (intel_cmd_emit_blit(&c, dst->gpu_vaddr, dst->stride,
                            src->gpu_vaddr, src->stride,
                            s.x, s.y, d.x, d.y, s.w, s.h)) return -1;
    intel_fence_t f;
    if (intel_gpu_submit(&c, &f)) return -1;
    return intel_fence_wait_sleep(&f, 0);
}

int intel_test_blit_run(void) {
    if (!intel_bcs_is_available()) {
        intel_cmd_t c;
        intel_cmd_begin(&c);
        if (intel_cmd_emit_blit(&c, 0x100000ULL, 64, 0x200000ULL, 64,
                                8, 8, 32, 32, 16, 16) == 0 &&
            c.n == INTEL_XY_COPY_LEN) {
            serial_print("[intel_blit] GPU BLIT: SKIP (no BCS)\n");
            return 1;
        }
        serial_print("[intel_blit] GPU BLIT: FAIL (builder)\n");
        return -1;
    }

    int src = intel_gpu_buffer_create(BLIT_W, BLIT_H);
    int dst = intel_gpu_buffer_create(BLIT_W, BLIT_H);
    if (src < 0 || dst < 0) goto alloc_fail;
    if (intel_gpu_buffer_map_gpu(src) || intel_gpu_buffer_map_gpu(dst))
        goto alloc_fail;

    intel_gpu_buffer_t* sb = intel_gpu_buffer_get(src);
    intel_gpu_buffer_t* db = intel_gpu_buffer_get(dst);
    for (uint32_t y = 0; y < BLIT_H; y++)
        for (uint32_t x = 0; x < BLIT_W; x++) {
            buf_write(sb, x, y, 0xFF000000u | (y * BLIT_W + x));
            buf_write(db, x, y, 0);
        }

    // 1. Full 1:1.
    if (hw_blit(db, sb, 0, 0, 0, 0, BLIT_W, BLIT_H)) goto fail;
    for (uint32_t y = 0; y < BLIT_H; y++)
        for (uint32_t x = 0; x < BLIT_W; x++)
            if (buf_read(db, x, y) != (0xFF000000u | (y * BLIT_W + x))) goto fail;

    // 2. Sub-rect with offset: src(8,8,16,16) -> dst(32,32).
    for (uint32_t y = 0; y < BLIT_H; y++)
        for (uint32_t x = 0; x < BLIT_W; x++) buf_write(db, x, y, 0);
    if (hw_blit(db, sb, 8, 8, 32, 32, 16, 16)) goto fail;
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++) {
            uint32_t want = 0xFF000000u | ((8 + y) * BLIT_W + (8 + x));
            if (buf_read(db, 32 + x, 32 + y) != want) goto fail;
        }
    if (buf_read(db, 0, 0) != 0 || buf_read(db, 63, 63) != 0) goto fail;

    // 3. Two blits, one submission.
    for (uint32_t y = 0; y < BLIT_H; y++)
        for (uint32_t x = 0; x < BLIT_W; x++) buf_write(db, x, y, 0);
    {
        intel_cmd_t c;
        intel_cmd_begin(&c);
        if (intel_cmd_emit_blit(&c, db->gpu_vaddr, db->stride,
                                sb->gpu_vaddr, sb->stride,
                                0, 0, 0, 32, 32, 32)) goto fail;
        if (intel_cmd_emit_blit(&c, db->gpu_vaddr, db->stride,
                                sb->gpu_vaddr, sb->stride,
                                32, 32, 32, 0, 32, 32)) goto fail;
        intel_fence_t f2;
        if (intel_gpu_submit(&c, &f2) || intel_fence_wait_sleep(&f2, 0)) goto fail;
        // dst(0,32) == src(0,0); dst(32,0) == src(32,32).
        if (buf_read(db, 0, 32) != (0xFF000000u | 0)) goto fail;
        if (buf_read(db, 32, 0) != (0xFF000000u | (32 * BLIT_W + 32))) goto fail;
        if (buf_read(db, 0, 0) != 0) goto fail;
    }

    intel_gpu_buffer_destroy(src);
    intel_gpu_buffer_destroy(dst);
    serial_print("[intel_blit] GPU BLIT: PASS\n");
    return 0;

alloc_fail:
    if (src >= 0) intel_gpu_buffer_destroy(src);
    if (dst >= 0) intel_gpu_buffer_destroy(dst);
    serial_print("[intel_blit] GPU BLIT: FAIL (alloc/map)\n");
    return -1;
fail:
    intel_gpu_buffer_destroy(src);
    intel_gpu_buffer_destroy(dst);
    serial_print("[intel_blit] GPU BLIT: FAIL\n");
    return -1;
}
