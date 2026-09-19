// ============================================================
// Gen12 hardware FILL tests — AL-10
// (graphics/backend/intel_gen12_test_fill.c)
//
// Cases: full surface, small rect, edge rect, unaligned rect,
// two fills sharing one batch (multi-op). Rect pixels must equal
// the fill color; sampled outside pixels must stay zero.
// ============================================================

#include "intel_gen12_test_fill.h"
#include "intel_gen12.h"
#include "intel_gen12_batch.h"
#include "intel_gen12_fence.h"
#include "intel_gen12_submit.h"
#include "intel_gen12_ppgtt.h"
#include "intel_gpu_alloc.h"
#include "intel_gen12_test_util.h"
#include <string.h>

extern void serial_print(const char* s);

#define buf_write gen12t_write
#define buf_read gen12t_read
#define buf_flush gen12t_flush
#define run_batch gen12t_run_batch

static int fill_case(const char* name, uint32_t W, uint32_t H,
                     uint32_t color, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h, int check_outside) {
    int id = intel_gpu_buffer_create(W, H);
    if (id < 0) {
        serial_print(name); serial_print(": FAIL (alloc)\n");
        return -1;
    }
    if (intel_gpu_buffer_map_gpu(id) != 0) {
        intel_gpu_buffer_destroy(id);
        serial_print(name); serial_print(": FAIL (map)\n");
        return -1;
    }
    intel_gpu_buffer_t* db = intel_gpu_buffer_get(id);
    if (!db) {
        intel_gpu_buffer_destroy(id);
        serial_print(name); serial_print(": FAIL (get)\n");
        return -1;
    }
    for (uint32_t yy = 0; yy < H; yy++)
        for (uint32_t xx = 0; xx < W; xx++)
            buf_write(db, xx, yy, 0);

    // NOTE: PPGTT mirror-sync happens inside commit (single owner).
    gen12_fence_t f;
    if (gen12_fence_alloc(&f) != 0) {
        intel_gpu_buffer_destroy(id);
        serial_print(name); serial_print(": BLOCKED (fence)\n");
        return -1;
    }
    gen12_batch_t b;
    if (gen12_batch_begin(&b) != 0) {
        intel_gpu_buffer_destroy(id);
        serial_print(name); serial_print(": FAIL (batch)\n");
        return -1;
    }
    int ok = gen12_batch_emit_fill(&b, db->gpu_vaddr, db->stride,
                                   color, x, y, w, h);
    if (ok == 0) ok = gen12_fence_emit(&b, &f);
    if (ok == 0) ok = gen12_batch_end(&b);
    if (ok != 0) {
        gen12_batch_free(&b);
        intel_gpu_buffer_destroy(id);
        serial_print(name); serial_print(": FAIL (emit)\n");
        return -1;
    }
    int rc = run_batch(name, &b, &f);
    gen12_batch_free(&b);
    if (rc == 0) {
        buf_flush(db);
        for (uint32_t yy = y; yy < y + h && rc == 0; yy++)
            for (uint32_t xx = x; xx < x + w; xx++)
                if (buf_read(db, xx, yy) != color) { rc = -1; break; }
        if (rc == 0 && check_outside) {
            // Sampled survivors: corners + opposite edge midpoints.
            uint32_t px[6][2] = {
                {0, 0}, {W - 1, 0}, {0, H - 1},
                {W - 1, H - 1}, {W / 2, 0}, {0, H / 2}
            };
            for (int i = 0; i < 6 && rc == 0; i++) {
                uint32_t px_ = px[i][0], py = px[i][1];
                if (px_ >= x && px_ < x + w && py >= y && py < y + h)
                    continue;   // inside the fill, skip
                if (buf_read(db, px_, py) != 0) rc = -1;
            }
        }
        serial_print(name);
        serial_print(rc == 0 ? ": PASS\n" : ": FAIL (mismatch)\n");
    }
    intel_gpu_buffer_destroy(id);
    return rc;
}

// Two fills, two colors, one batch → proves multi-op batches + ordering.
static int fill_multi(void) {
    const char* name = "[gen12_fill] multi";
    int id = intel_gpu_buffer_create(64, 64);
    if (id < 0) {
        serial_print(name); serial_print(": FAIL (alloc)\n");
        return -1;
    }
    if (intel_gpu_buffer_map_gpu(id) != 0) {
        intel_gpu_buffer_destroy(id);
        serial_print(name); serial_print(": FAIL (map)\n");
        return -1;
    }
    intel_gpu_buffer_t* db = intel_gpu_buffer_get(id);
    for (uint32_t yy = 0; yy < 64; yy++)
        for (uint32_t xx = 0; xx < 64; xx++)
            buf_write(db, xx, yy, 0);
    // NOTE: PPGTT mirror-sync happens inside commit (single owner).
    gen12_fence_t f;
    if (gen12_fence_alloc(&f) != 0) {
        intel_gpu_buffer_destroy(id);
        serial_print(name); serial_print(": BLOCKED (fence)\n");
        return -1;
    }
    gen12_batch_t b;
    if (gen12_batch_begin(&b) != 0) {
        intel_gpu_buffer_destroy(id);
        serial_print(name); serial_print(": FAIL (batch)\n");
        return -1;
    }
    int ok = gen12_batch_emit_fill(&b, db->gpu_vaddr, db->stride,
                                   0xFFFF0000u, 0, 0, 32, 64);
    if (ok == 0) ok = gen12_batch_emit_fill(&b, db->gpu_vaddr, db->stride,
                                            0xFF0000FFu, 32, 0, 32, 64);
    if (ok == 0) ok = gen12_fence_emit(&b, &f);
    if (ok == 0) ok = gen12_batch_end(&b);
    if (ok != 0) {
        gen12_batch_free(&b);
        intel_gpu_buffer_destroy(id);
        serial_print(name); serial_print(": FAIL (emit)\n");
        return -1;
    }
    int rc = run_batch(name, &b, &f);
    gen12_batch_free(&b);
    if (rc == 0) {
        buf_flush(db);
        for (uint32_t yy = 0; yy < 64 && rc == 0; yy++)
            for (uint32_t xx = 0; xx < 64; xx++) {
                uint32_t want = xx < 32 ? 0xFFFF0000u : 0xFF0000FFu;
                if (buf_read(db, xx, yy) != want) { rc = -1; break; }
            }
        serial_print(name);
        serial_print(rc == 0 ? ": PASS\n" : ": FAIL (mismatch)\n");
    }
    intel_gpu_buffer_destroy(id);
    return rc;
}

static int g_fill_done = 0;

void gen12_test_fill_run(void) {
    if (g_fill_done) return;
    g_fill_done = 1;
    int pass = 0;
    if (fill_case("[gen12_fill] full", 64, 64, 0xFF112233u,
                  0, 0, 64, 64, 0) == 0) pass++;
    if (fill_case("[gen12_fill] small", 64, 64, 0xFF445566u,
                  8, 8, 16, 16, 1) == 0) pass++;
    if (fill_case("[gen12_fill] edge", 64, 64, 0xFF778899u,
                  56, 56, 8, 8, 1) == 0) pass++;
    if (fill_case("[gen12_fill] unaligned", 64, 64, 0xFFAABBCCu,
                  7, 5, 24, 24, 1) == 0) pass++;
    if (fill_multi() == 0) pass++;
    serial_print("[gen12_fill] FILL: ");
    serial_print(pass == 5 ? "PASS (5/5)\n" : "done, see cases above\n");
}
