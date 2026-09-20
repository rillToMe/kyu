// ============================================================
// Gen12 hardware BLIT tests — AL-11
// (graphics/backend/intel/intel_gen12_test_blit.c)
//
// Same pipeline as AL-9 COPY (offset-capable XY variant).
// The chained case (A→B then B→C in one batch) proves in-order
// execution inside a single submit: C must equal A's pattern.
// ============================================================

#include "intel_gen12_test_blit.h"
#include "intel_gen12.h"
#include "intel_gen12_batch.h"
#include "intel_gen12_fence.h"
#include "intel_gen12_submit.h"
#include "intel_gen12_ppgtt.h"
#include "intel_gpu_alloc.h"
#include "intel_rect.h"
#include "intel_gen12_test_util.h"
#include <string.h>

extern void serial_print(const char* s);

#define buf_write gen12t_write
#define buf_read gen12t_read
#define buf_flush gen12t_flush
#define run_batch gen12t_run_batch

static int blit_case(const char* name,
                     uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                     uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                     uint32_t w, uint32_t h) {
    int src = intel_gpu_buffer_create(sw, sh);
    int dst = intel_gpu_buffer_create(dw, dh);
    if (src < 0 || dst < 0) {
        if (src >= 0) intel_gpu_buffer_destroy(src);
        if (dst >= 0) intel_gpu_buffer_destroy(dst);
        serial_print(name); serial_print(": FAIL (alloc)\n");
        return -1;
    }
    if (intel_gpu_buffer_map_gpu(src) != 0 ||
        intel_gpu_buffer_map_gpu(dst) != 0) {
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        serial_print(name); serial_print(": FAIL (map)\n");
        return -1;
    }
    intel_gpu_buffer_t* sb = intel_gpu_buffer_get(src);
    intel_gpu_buffer_t* db = intel_gpu_buffer_get(dst);
    if (!sb || !db) {
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        serial_print(name); serial_print(": FAIL (get)\n");
        return -1;
    }

    intel_rect_t rs = {sx, sy, w, h}, rd = {dx, dy, w, h};
    if (intel_blit_validate(db, sb, &rs, &rd) != 0) {
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        serial_print(name); serial_print(": FAIL (validate)\n");
        return -1;
    }

    for (uint32_t y = 0; y < sh; y++)
        for (uint32_t x = 0; x < sw; x++)
            buf_write(sb, x, y, 0xFF000000u | ((y * sw + x) & 0xFFFFFFu));
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++)
            buf_write(db, x, y, 0);
    buf_flush(sb);
    // NOTE: PPGTT mirror-sync happens inside commit (single owner).
    gen12_fence_t f;
    if (gen12_fence_alloc(&f) != 0) {
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        serial_print(name); serial_print(": BLOCKED (fence)\n");
        return -1;
    }
    gen12_batch_t b;
    if (gen12_batch_begin(&b) != 0) {
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        serial_print(name); serial_print(": FAIL (batch)\n");
        return -1;
    }
    int ok = gen12_batch_emit_blit(&b, db->gpu_vaddr, db->stride,
                                   sb->gpu_vaddr, sb->stride,
                                   sx, sy, dx, dy, w, h);
    if (ok == 0) ok = gen12_fence_emit(&b, &f);
    if (ok == 0) ok = gen12_batch_end(&b);
    if (ok != 0) {
        gen12_batch_free(&b);
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        serial_print(name); serial_print(": FAIL (emit)\n");
        return -1;
    }
    int rc = run_batch(name, &b, &f);
    gen12_batch_free(&b);
    if (rc == 0) {
        buf_flush(db);
        for (uint32_t y = 0; y < h && rc == 0; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t want =
                    0xFF000000u | (((sy + y) * sw + (sx + x)) & 0xFFFFFFu);
                if (buf_read(db, dx + x, dy + y) != want) { rc = -1; break; }
            }
        serial_print(name);
        serial_print(rc == 0 ? ": PASS\n" : ": FAIL (mismatch)\n");
    }
    intel_gpu_buffer_destroy(src);
    intel_gpu_buffer_destroy(dst);
    return rc;
}

// A→B then B→C inside one batch: C must equal A's pattern.
static int blit_chain(void) {
    const char* name = "[gen12_blit] chain";
    int a = intel_gpu_buffer_create(64, 64);
    int b = intel_gpu_buffer_create(64, 64);
    int c = intel_gpu_buffer_create(64, 64);
    if (a < 0 || b < 0 || c < 0) {
        if (a >= 0) intel_gpu_buffer_destroy(a);
        if (b >= 0) intel_gpu_buffer_destroy(b);
        if (c >= 0) intel_gpu_buffer_destroy(c);
        serial_print(name); serial_print(": FAIL (alloc)\n");
        return -1;
    }
    if (intel_gpu_buffer_map_gpu(a) != 0 ||
        intel_gpu_buffer_map_gpu(b) != 0 ||
        intel_gpu_buffer_map_gpu(c) != 0) {
        intel_gpu_buffer_destroy(a);
        intel_gpu_buffer_destroy(b);
        intel_gpu_buffer_destroy(c);
        serial_print(name); serial_print(": FAIL (map)\n");
        return -1;
    }
    intel_gpu_buffer_t* ba = intel_gpu_buffer_get(a);
    intel_gpu_buffer_t* bb = intel_gpu_buffer_get(b);
    intel_gpu_buffer_t* bc = intel_gpu_buffer_get(c);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++)
            buf_write(ba, x, y, 0xFF000000u | ((y * 64 + x) & 0xFFFFFFu));
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            buf_write(bb, x, y, 0);
            buf_write(bc, x, y, 0);
        }
    buf_flush(ba);
    // NOTE: PPGTT mirror-sync happens inside commit (single owner).
    gen12_fence_t f;
    if (gen12_fence_alloc(&f) != 0) {
        intel_gpu_buffer_destroy(a);
        intel_gpu_buffer_destroy(b);
        intel_gpu_buffer_destroy(c);
        serial_print(name); serial_print(": BLOCKED (fence)\n");
        return -1;
    }
    gen12_batch_t bt;
    if (gen12_batch_begin(&bt) != 0) {
        intel_gpu_buffer_destroy(a);
        intel_gpu_buffer_destroy(b);
        intel_gpu_buffer_destroy(c);
        serial_print(name); serial_print(": FAIL (batch)\n");
        return -1;
    }
    int ok = gen12_batch_emit_blit(&bt, bb->gpu_vaddr, bb->stride,
                                   ba->gpu_vaddr, ba->stride,
                                   0, 0, 0, 0, 64, 64);
    if (ok == 0) ok = gen12_batch_emit_blit(&bt, bc->gpu_vaddr, bc->stride,
                                            bb->gpu_vaddr, bb->stride,
                                            0, 0, 0, 0, 64, 64);
    if (ok == 0) ok = gen12_fence_emit(&bt, &f);
    if (ok == 0) ok = gen12_batch_end(&bt);
    if (ok != 0) {
        gen12_batch_free(&bt);
        intel_gpu_buffer_destroy(a);
        intel_gpu_buffer_destroy(b);
        intel_gpu_buffer_destroy(c);
        serial_print(name); serial_print(": FAIL (emit)\n");
        return -1;
    }
    int rc = run_batch(name, &bt, &f);
    gen12_batch_free(&bt);
    if (rc == 0) {
        buf_flush(bc);
        for (uint32_t y = 0; y < 64 && rc == 0; y++)
            for (uint32_t x = 0; x < 64; x++) {
                uint32_t want = 0xFF000000u | ((y * 64 + x) & 0xFFFFFFu);
                if (buf_read(bc, x, y) != want) { rc = -1; break; }
            }
        serial_print(name);
        serial_print(rc == 0 ? ": PASS\n" : ": FAIL (mismatch)\n");
    }
    intel_gpu_buffer_destroy(a);
    intel_gpu_buffer_destroy(b);
    intel_gpu_buffer_destroy(c);
    return rc;
}

static int g_blit_done = 0;

void gen12_test_blit_run(void) {
    if (g_blit_done) return;
    g_blit_done = 1;
    int pass = 0;
    if (blit_case("[gen12_blit] full", 64, 64, 64, 64,
                  0, 0, 0, 0, 64, 64) == 0) pass++;
    if (blit_case("[gen12_blit] subrect", 64, 64, 64, 64,
                  8, 8, 8, 8, 16, 16) == 0) pass++;
    if (blit_case("[gen12_blit] offset", 64, 64, 64, 64,
                  0, 0, 16, 16, 32, 32) == 0) pass++;
    if (blit_case("[gen12_blit] crossbuf", 64, 64, 128, 128,
                  0, 0, 32, 32, 64, 64) == 0) pass++;
    if (blit_chain() == 0) pass++;
    serial_print("[gen12_blit] BLIT: ");
    serial_print(pass == 5 ? "PASS (5/5)\n" : "done, see cases above\n");
}
