// ============================================================
// Intel failure-path self-test — Phase 22
// (graphics/backend/intel/intel_robust.c)
//
// Every case below must return safely. If any of these
// crashed, a userspace/compositor bug could take down the OS.
// Safe without HW: no GTT/BCS/MMIO touched (all callees fail
// closed before touching hardware when uninitialized).
// ============================================================

#include "intel_robust.h"
#include "intel_gpu_alloc.h"
#include "intel_gtt.h"
#include "intel_bcs.h"
#include "intel_cmd.h"
#include "intel_fence.h"
#include "intel_rect.h"

extern void serial_print(const char* s);

int intel_robust_selftest(void) {
    int fails = 0, total = 0;
#define TRY(expr) do { total++; if (!(expr)) fails++; } while (0)

    // --- Buffer allocator rejects ---
    TRY(intel_gpu_buffer_create(0, 64) < 0);
    TRY(intel_gpu_buffer_create(64, 0) < 0);
    TRY(intel_gpu_buffer_create(8193, 64) < 0);
    TRY(intel_gpu_buffer_create(0xFFFFFFFFu, 0xFFFFFFFFu) < 0);
    TRY(intel_gpu_buffer_get(-1) == 0);
    TRY(intel_gpu_buffer_get(0) == 0);
    TRY(intel_gpu_buffer_get(9999) == 0);
    TRY(intel_gpu_buffer_cpu_ptr(-1) == 0);
    TRY(intel_gpu_buffer_gpu_vaddr(-1) == 0);
    TRY(intel_gpu_buffer_stride(-1) == 0);
    TRY(intel_gpu_buffer_map_gpu(-1) != 0);
    intel_gpu_buffer_destroy(-1); total++;   // must not crash
    intel_gpu_buffer_unmap_gpu(-1); total++; // must not crash

    // --- GTT rejects (inactive without intel_init) ---
    TRY(intel_gtt_map_pages(0x100000ULL, 0, 1) == (uint32_t)-1);
    TRY(intel_gtt_map_pages(0, 0, 0) == (uint32_t)-1);
    intel_gtt_unmap(0, 1); total++; // must not crash
    intel_gtt_flush(); total++;     // must not crash

    // --- BCS rejects (unavailable without HW) ---
    TRY(intel_bcs_submit(0, 4) != 0);
    {
        uint32_t dw = 0;
        TRY(intel_bcs_submit(&dw, 0) != 0);
    }
    TRY(intel_bcs_is_busy() == 0);
    TRY(intel_bcs_wait_idle(0) == 0);

    // --- Command builders reject ---
    TRY(intel_cmd_emit_noop(0) != 0);
    TRY(intel_cmd_emit_bb_end(0) != 0);
    TRY(intel_cmd_emit_store(0, 0, 0) != 0);
    TRY(intel_cmd_emit_copy(0, 0, 0, 0, 0, 0, 0, 0, 0) != 0);
    TRY(intel_cmd_emit_fill(0, 0, 0, 0, 0, 0, 0, 0) != 0);
    TRY(intel_cmd_emit_blit(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) != 0);
    {
        intel_cmd_t c;
        intel_cmd_begin(&c);
        TRY(intel_cmd_emit_copy(&c, 0, 0, 0, 0, 0, 0, 0, 0) != 0); // w=0
        TRY(intel_cmd_submit(&c) != 0); // empty staging
    }
    TRY(intel_cmd_submit(0) != 0);

    // --- Fence rejects ---
    TRY(intel_fence_is_signaled(0) == 0);
    TRY(intel_fence_wait(0, 0) != 0);
    TRY(intel_fence_wait_sleep(0, 0) != 0);
    {
        intel_cmd_t c;
        intel_cmd_begin(&c);
        intel_fence_t f;
        TRY(intel_gpu_submit(0, &f) != 0);
        TRY(intel_gpu_submit(&c, 0) != 0);
    }

    // --- Rect gate rejects ---
    TRY(intel_rect_clip(0, 64, 64) != 0);
    TRY(intel_fill_validate(0, 0) != 0);
    TRY(intel_blit_validate(0, 0, 0, 0) != 0);

#undef TRY
    if (fails) {
        serial_print("[robust] ROBUST: FAIL\n");
        return -1;
    }
    serial_print("[robust] ROBUST: PASS\n");
    (void)total;
    return 0;
}
