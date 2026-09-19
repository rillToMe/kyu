// ============================================================
// Intel GPU FILL test — Phase 10
// (graphics/backend/intel_test_fill.c)
//
// One 64x64 buffer, per-page CPU access (PMM may be
// non-contiguous). Clipping done on CPU before emit —
// the GPU never sees out-of-bounds coords (Phase 12 rule).
// ============================================================

#include "intel_test_fill.h"
#include "intel_gpu_alloc.h"
#include "intel_rect.h"
#include "intel_regs.h"
#include "intel_bcs.h"
#include "intel_cmd.h"
#include "intel_fence.h"

extern uint64_t hhdm_offset;
extern void serial_print(const char* s);

#define FILL_W 64
#define FILL_H 64

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

static void buf_clear(intel_gpu_buffer_t* b) {
    for (uint32_t y = 0; y < FILL_H; y++)
        for (uint32_t x = 0; x < FILL_W; x++)
            buf_write(b, x, y, 0);
}

// Submit one fill + BB_END, wait. 0 ok.
// Clipped via Phase-12 gate (GPU never sees OOB).
static int hw_fill(intel_gpu_buffer_t* b, uint32_t color,
                   uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    intel_rect_t r = {x, y, w, h};
    if (intel_fill_validate(b, &r)) return (x >= FILL_W || y >= FILL_H) ? 0 : -1;
    intel_cmd_t c;
    intel_cmd_begin(&c);
    if (intel_cmd_emit_fill(&c, b->gpu_vaddr, b->stride, color, r.x, r.y, r.w, r.h)) return -1;
    intel_fence_t f;
    if (intel_gpu_submit(&c, &f)) return -1;
    return intel_fence_wait_sleep(&f, 0);
}

// Check rect == color and outside == bg. 0 match.
static int check_rect(intel_gpu_buffer_t* b, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t color, uint32_t bg) {
    for (uint32_t yy = 0; yy < FILL_H; yy++)
        for (uint32_t xx = 0; xx < FILL_W; xx++) {
            int inside = (xx >= x && xx < x + w && yy >= y && yy < y + h);
            uint32_t want = inside ? color : bg;
            if (buf_read(b, xx, yy) != want) return -1;
        }
    return 0;
}

int intel_test_fill_run(void) {
    if (!intel_bcs_is_available()) {
        // Builder-only check (no HW).
        intel_cmd_t c;
        intel_cmd_begin(&c);
        if (intel_cmd_emit_fill(&c, 0x100000ULL, 64, 0xFF00FF00, 0, 0, 8, 8) == 0 &&
            c.n == INTEL_XY_FILL_LEN) {
            serial_print("[intel_fill] GPU FILL: SKIP (no BCS)\n");
            return 1;
        }
        serial_print("[intel_fill] GPU FILL: FAIL (builder)\n");
        return -1;
    }

    int id = intel_gpu_buffer_create(FILL_W, FILL_H);
    if (id < 0 || intel_gpu_buffer_map_gpu(id) != 0) {
        serial_print("[intel_fill] GPU FILL: FAIL (alloc/map)\n");
        if (id >= 0) intel_gpu_buffer_destroy(id);
        return -1;
    }
    intel_gpu_buffer_t* b = intel_gpu_buffer_get(id);

    // 1. Full-buffer fill.
    buf_clear(b);
    if (hw_fill(b, 0xFF112233, 0, 0, FILL_W, FILL_H) ||
        check_rect(b, 0, 0, FILL_W, FILL_H, 0xFF112233, 0xFF112233)) goto fail;

    // 2. Small rect on cleared buffer.
    buf_clear(b);
    if (hw_fill(b, 0xFF445566, 8, 8, 16, 16) ||
        check_rect(b, 8, 8, 16, 16, 0xFF445566, 0)) goto fail;

    // 3. Edge rect (56,56,16,16) clipped to 8x8.
    buf_clear(b);
    if (hw_fill(b, 0xFF778899, 56, 56, 16, 16) ||
        check_rect(b, 56, 56, 8, 8, 0xFF778899, 0)) goto fail;

    // 4. Unaligned rect (odd x/w).
    buf_clear(b);
    if (hw_fill(b, 0xFFAABBCC, 3, 5, 7, 11) ||
        check_rect(b, 3, 5, 7, 11, 0xFFAABBCC, 0)) goto fail;

    // 5. Two rects, one submission.
    buf_clear(b);
    {
        intel_cmd_t c;
        intel_cmd_begin(&c);
        if (intel_cmd_emit_fill(&c, b->gpu_vaddr, b->stride, 0xFF101010, 0, 0, 32, 32)) goto fail;
        if (intel_cmd_emit_fill(&c, b->gpu_vaddr, b->stride, 0xFF202020, 32, 32, 32, 32)) goto fail;
        intel_fence_t f2;
        if (intel_gpu_submit(&c, &f2) || intel_fence_wait_sleep(&f2, 0)) goto fail;
        if (buf_read(b, 0, 0) != 0xFF101010) goto fail;
        if (buf_read(b, 63, 63) != 0xFF202020) goto fail;
        if (buf_read(b, 0, 63) != 0) goto fail;
        if (buf_read(b, 63, 0) != 0) goto fail;
    }

    intel_gpu_buffer_destroy(id);
    serial_print("[intel_fill] GPU FILL: PASS\n");
    return 0;
fail:
    intel_gpu_buffer_destroy(id);
    serial_print("[intel_fill] GPU FILL: FAIL\n");
    return -1;
}
