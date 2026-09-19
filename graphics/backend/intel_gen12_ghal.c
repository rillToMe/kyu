// ============================================================
// GHAL-level integration check — AL-14
// (graphics/backend/intel_gen12_ghal.c)
//
// Runs after ghal_init selected a backend (same hook pattern as
// the Phase-20 bench and Phase-22 robust self-test). Verdict from
// the AL-13 dispatch counters: HW path taken vs CPU fallback.
// Compositor REDESIGN (KWM canvases → GHAL surfaces) is explicitly
// out of scope — window composition stays CPU; this check covers
// the GHAL op path the compositor and clients actually call.
// ============================================================

#include "intel_gen12_ghal.h"
#include "intel_gen12_submit.h"
#include "ghal.h"
#include <string.h>

extern void serial_print(const char* s);

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void gen12_ghal_check(void) {
    if (!streq(ghal_active_backend_name(), "intel")) {
        serial_print("[gen12_ghal] SKIP (backend not intel)\n");
        return;
    }
    uint64_t h0 = 0, f0 = 0, h1 = 0, f1 = 0;
    gen12_op_counts(&h0, &f0);

    ghal_surface_t* A = ghal_surface_create(64, 64, GHAL_FMT_XRGB8888);
    ghal_surface_t* B = ghal_surface_create(64, 64, GHAL_FMT_XRGB8888);
    if (!A || !B) {
        if (A) ghal_surface_destroy(A);
        if (B) ghal_surface_destroy(B);
        serial_print("[gen12_ghal] FAIL (alloc)\n");
        return;
    }
    ghal_rect_t full = {0, 0, 64, 64};
    ghal_fill_rect(A, full, 0xFF112233u);
    ghal_blit(B, full, A, full);
    ghal_present(B, &full);
    ghal_surface_destroy(A);
    ghal_surface_destroy(B);

    gen12_op_counts(&h1, &f1);
    if (h1 > h0) {
        serial_print("[gen12_ghal] integration: HW path\n");
    } else {
        // Either CPU fallback or the legacy Gen8-11 BCS path (which
        // has no Gen12 counters by design) — both are non-Gen12.
        serial_print("[gen12_ghal] integration: non-Gen12 path\n");
    }
}
