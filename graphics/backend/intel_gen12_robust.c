// ============================================================
// Gen12 robustness suite — AL-16
// (graphics/backend/intel_gen12_robust.c)
//
// Two halves: pure-API rejections (run everywhere, zero HW
// touch) and fail-closed proofs (only when the engine is NOT
// live — on a live engine these calls would submit, so they
// are skipped there by design, not by omission).
// ============================================================

#include "intel_gen12_robust.h"
#include "intel_gen12.h"
#include "intel_gen12_batch.h"
#include "intel_gen12_fence.h"
#include "intel_gen12_submit.h"
#include "intel_gen12_ctx.h"
#include "intel_rect.h"
#include "intel_gpu_alloc.h"

extern void serial_print(const char* s);

static int g_robust_done = 0;
static int g_fail = 0;

#define TRY(cond) do { if (!(cond)) { g_fail = 1; goto done; } } while (0)

void gen12_robust_selftest(void) {
    if (g_robust_done) return;
    g_robust_done = 1;
    g_fail = 0;

    static uint32_t scratch[GEN12_BATCH_DWORDS];
    gen12_batch_t t;
    t.cpu = scratch; t.phys = 0; t.gpu = 0;
    t.n = 0; t.sealed = 0; t.backed = 0;

    // --- Null / empty / sealed batches ---
    TRY(gen12_batch_emit_copy(0, 0x100000ULL, 64, 0x200000ULL, 64,
                              0, 0, 8, 8) != 0);
    TRY(gen12_batch_emit_fill(0, 0x100000ULL, 64, 0, 0, 0, 8, 8) != 0);
    TRY(gen12_batch_emit_blit(0, 0x100000ULL, 64, 0x200000ULL, 64,
                              0, 0, 0, 0, 8, 8) != 0);
    TRY(gen12_batch_end(0) != 0);
    TRY(gen12_batch_begin(0) != 0);
    gen12_batch_free(0);   // must tolerate NULL
    if (gen12_batch_gpu_addr(0) != 0) { g_fail = 1; goto done; }
    if (gen12_batch_gpu_addr(&t) != 0) { g_fail = 1; goto done; }  // empty

    // --- Geometry rejections ---
    TRY(gen12_batch_emit_fill(&t, 0x100000ULL, 64, 1, 0, 0, 0, 8) != 0);
    TRY(gen12_batch_emit_fill(&t, 0x100000ULL, 64, 1, 0, 0, 8, 0) != 0);
    TRY(gen12_batch_emit_fill(&t, 0x100000ULL, 64, 1,
                              0xFFFFu, 0, 2, 8) != 0);       // x+w wrap
    TRY(gen12_batch_emit_fill(&t, 0x100000ULL, 64, 1,
                              0, 0xFFFFu, 8, 2) != 0);       // y+h wrap
    TRY(gen12_batch_emit_fill(&t, 0x100000ULL, 0, 1, 0, 0, 8, 8) != 0);
    TRY(gen12_batch_emit_fill(&t, 0x100000ULL, 0x4000u, 1, 0, 0, 8, 8) != 0);
    TRY(gen12_batch_emit_copy(&t, 0x100000ULL, 64, 0x200000ULL, 64,
                              0, 0, 0, 8) != 0);

    // --- Address rejections (page-checked in-window) ---
    TRY(gen12_batch_emit_fill(&t, 0x100001ULL, 64, 1, 0, 0, 8, 8) != 0);
    TRY(gen12_batch_emit_fill(&t, 0x800000ULL, 64, 1, 0, 0, 8, 8) != 0);
    TRY(gen12_batch_emit_copy(&t, 0x100000ULL, 64, 0x900000ULL, 64,
                              0, 0, 8, 8) != 0);

    // --- Capacity: fill the scratch, next emit must refuse ---
    {
        gen12_batch_t f;
        f.cpu = scratch; f.phys = 0; f.gpu = 0;
        f.n = 0; f.sealed = 0; f.backed = 0;
        uint32_t k;
        for (k = 0; k < 200; k++) {
            if (gen12_batch_emit_fill(&f, 0x100000ULL, 64, (uint32_t)k,
                                      0, 0, 8, 8) != 0) break;
        }
        TRY(k < 200);   // 200x7 DW > 1024: must stop early
        TRY(gen12_batch_end(&f) == 0);
        TRY(gen12_batch_emit_fill(&f, 0x100000ULL, 64, 0, 0, 0, 8, 8) != 0);
        TRY(gen12_batch_end(&f) != 0);   // double seal refused
    }

    // --- Invalid fences ---
    {
        gen12_fence_t bad;
        bad.seqno = 0;
        TRY(gen12_fence_emit(&t, &bad) != 0);
        TRY(gen12_fence_emit(0, &bad) != 0);
        TRY(!gen12_fence_is_signaled(&bad));
        TRY(gen12_fence_wait(&bad, 10) != 0);
        TRY(gen12_fence_alloc(0) != 0);
    }

    // --- Validators reject NULL buffers (Phase-12 gate, Gen12 callers) ---
    {
        intel_rect_t r = {0, 0, 8, 8}, s = {0, 0, 8, 8}, d = {0, 0, 8, 8};
        TRY(intel_fill_validate(0, &r) != 0);
        TRY(intel_blit_validate(0, 0, &s, &d) != 0);
        TRY(intel_gen12_ctx_validate(0) != 0);
        static uint32_t zeroimg[1024];
        for (uint32_t i = 0; i < 1024; i++) zeroimg[i] = 0;
        TRY(intel_gen12_ctx_validate(zeroimg) != 0);  // not vacuous
        TRY(intel_gen12_vm_valid(0x1000, 0) != 0);
    }

    // --- Destroyed buffers vanish from the pool ---
    {
        int id = intel_gpu_buffer_create(64, 64);
        TRY(id > 0);
        TRY(intel_gpu_buffer_get(id) != 0);
        TRY(intel_gpu_buffer_get(-1) == 0);
        intel_gpu_buffer_destroy(id);
        TRY(intel_gpu_buffer_get(id) == 0);
        intel_gpu_buffer_destroy(id);   // double destroy tolerated
        TRY(intel_gpu_buffer_get(9999) == 0);
    }

    // --- Fail-closed proofs: only valid when NOT live (on a live
    // engine these would submit, so asserting failure there would
    // itself be the bug). Dead engine ⇒ every door returns -1 and
    // nothing touches HW.
    if (!gen12_is_live()) {
        uint32_t dw[5] = {0, 0, 0, 0, 0};
        TRY(gen12_submit_commit(0, 5) != 0);
        TRY(gen12_submit_commit(dw, 0) != 0);
        TRY(gen12_submit_commit(dw, 2000) != 0);
        TRY(gen12_submit_commit(dw, 5) != 0);   // ring down or no ctx
    } else {
        serial_print("[gen12_robust] SKIP fail-closed (engine live)\n");
    }

done:
    serial_print(g_fail ? "[gen12_robust] robust: FAIL\n"
                        : "[gen12_robust] robust: PASS\n");
}
