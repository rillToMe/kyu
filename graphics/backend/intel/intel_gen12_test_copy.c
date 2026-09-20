// ============================================================
// Gen12 hardware COPY tests — AL-9
// (graphics/backend/intel/intel_gen12_test_copy.c)
//
// Pipeline per case: pool buffers → Phase-12 validate → pattern →
// clflush src → PPGTT sync → batch XY_COPY + STORE + END → ring →
// commit/ELSP → fence → clflush dst → CPU validate.
//
// Coherency notes: CPU writes (pattern) are flushed so the GPU
// reads DRAM; after the fence the dst pages are flushed so the
// CPU validation reads what the GPU wrote (never trust a WB
// cache line across a GPU write). x86 CLFLUSH, no HW knowledge.
// ============================================================

#include "intel_gen12_test_copy.h"
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

// One case. Prints its own verdict. 0 pass, -1 fail/blocked.
static int copy_case(const char* name,
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
    buf_flush(sb);   // pattern visible to GPU
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

    // Commit the validated batch through the single owner (private
    // staging → atomic ring copy + submit; safe under concurrency).
    // NOTE: commit BEFORE free — free() clears b.cpu.
    int submitted = gen12_submit_commit(b.cpu, b.n);
    gen12_batch_free(&b);
    if (submitted != 0) {
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        serial_print(name); serial_print(": BLOCKED (submit)\n");
        return -1;
    }

    int rc = 0;
    if (gen12_fence_wait(&f, 0) != 0) {
        serial_print(name); serial_print(": BLOCKED (no completion)\n");
        rc = -1;
    } else {
        buf_flush(db);   // drop CPU cache before validating GPU output
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

static int g_copy_done = 0;

void gen12_test_copy_run(void) {
    if (g_copy_done) return;
    g_copy_done = 1;
    int pass = 0, total = 5;
    if (copy_case("[gen12_copy] aligned64", 64, 64, 64, 64,
                  0, 0, 0, 0, 64, 64) == 0) pass++;
    if (copy_case("[gen12_copy] unaligned", 64, 64, 64, 64,
                  7, 5, 3, 9, 24, 24) == 0) pass++;
    if (copy_case("[gen12_copy] subregion", 64, 64, 64, 64,
                  16, 16, 32, 32, 16, 16) == 0) pass++;
    if (copy_case("[gen12_copy] page-x", 64, 64, 64, 64,
                  0, 14, 0, 14, 64, 4) == 0) pass++;
    if (copy_case("[gen12_copy] multi128", 128, 128, 128, 128,
                  0, 0, 0, 0, 128, 128) == 0) pass++;
    serial_print("[gen12_copy] COPY: ");
    serial_print(pass == total ? "PASS (5/5)\n" : "done, see cases above\n");
}
