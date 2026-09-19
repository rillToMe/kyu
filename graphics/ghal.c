// ============================================================
// Graphics HAL — dispatch, registry & backend selection
// (graphics/ghal.c)
//
// Menyimpan daftar backend, memilih backend aktif saat ghal_init()
// (urutan tetap: virtio-gpu → software, roadmap §6.1), dan me-route
// semua panggilan compositor ke vtable backend aktif. Tidak pernah
// menyentuh MMIO/register GPU — itu urusan driver core.
//
// Concurrency: registry/selection di-lock; panggilan ghal_* ke backend
// aktif diasumsikan single-context per frame (compositor), konsisten
// dengan model v1 di roadmap §6.6.
// ============================================================

#include "ghal.h"
#include "backend/intel_bench.h"
#include "backend/intel_robust.h"
#include "spinlock.h"
#include <stddef.h>   // NULL

#define GHAL_MAX_BACKENDS 8

static const ghal_backend_ops_t* g_backends[GHAL_MAX_BACKENDS];
static const ghal_backend_ops_t* g_active;
static spinlock_t g_lock = SPINLOCK_INIT;
static const char* g_last_error = "";

// Didefinisikan di graphics/backend/software.c, virtio_gpu.c, intel_init.c.
// software tidak pernah gagal.
extern const ghal_backend_ops_t software_backend_ops;
extern const ghal_backend_ops_t virtio_gpu_backend_ops;
extern const ghal_backend_ops_t intel_backend_ops;

int ghal_register_backend(const ghal_backend_ops_t* ops) {
    if (ops == NULL || ops->init == NULL || ops->surface_create == NULL ||
        ops->surface_destroy == NULL || ops->surface_upload == NULL ||
        ops->fill_rect == NULL || ops->blit == NULL || ops->present == NULL) {
        g_last_error = "ghal_register_backend: incomplete ops";
        return -1;
    }
    uint64_t flags = spinlock_lock_irqsave(&g_lock);
    for (int i = 0; i < GHAL_MAX_BACKENDS; i++) {
        if (g_backends[i] == NULL) {
            g_backends[i] = ops;
            spinlock_unlock_irqrestore(&g_lock, flags);
            return 0;
        }
    }
    spinlock_unlock_irqrestore(&g_lock, flags);
    g_last_error = "ghal_register_backend: table full";
    return -1;
}

int ghal_init(void) {
    uint64_t flags = spinlock_lock_irqsave(&g_lock);
    if (g_active != NULL) {
        spinlock_unlock_irqrestore(&g_lock, flags);
        return 0;
    }

    // Urutan tetap: virtio-gpu → intel → software fallback.
    const ghal_backend_ops_t* order[GHAL_MAX_BACKENDS];
    int n = 0;
    order[n++] = &virtio_gpu_backend_ops;   // coba dulu
    order[n++] = &intel_backend_ops;         // Intel iGPU via PCI
    order[n++] = &software_backend_ops;     // fallback tak pernah gagal

    for (int i = 0; i < n; i++) {
        const ghal_backend_ops_t* ops = order[i];
        if (ops->init && ops->init() == 0) {
            g_active = ops;
            g_last_error = "";
            spinlock_unlock_irqrestore(&g_lock, flags);
            ghal_diag_dump();   // Phase 15: roadmap-format GPU report
            intel_bench_run();  // Phase 20: always-on benchmark
            intel_robust_selftest(); // Phase 22: failure-path guards
            return 0;
        }
    }

    g_active = NULL;
    g_last_error = "ghal_init: no backend initialized";
    spinlock_unlock_irqrestore(&g_lock, flags);
    return -1;
}

void ghal_shutdown(void) {
    uint64_t flags = spinlock_lock_irqsave(&g_lock);
    if (g_active && g_active->shutdown) g_active->shutdown();
    g_active = NULL;
    spinlock_unlock_irqrestore(&g_lock, flags);
}

const char* ghal_active_backend_name(void) {
    return g_active ? g_active->name : "(none)";
}

uint32_t ghal_capabilities(void) {
    return g_active ? g_active->capabilities : 0;
}

const char* ghal_last_error(void) {
    return g_last_error;
}

// --- Acceleration info (Phase 15). NULL-safe, backend-agnostic. ---

int ghal_acceleration_enabled(void) {
    if (!g_active || !g_active->acceleration_enabled) return 0;
    return g_active->acceleration_enabled() ? 1 : 0;
}

const char* ghal_engine_name(void) {
    if (!g_active || !g_active->engine_name) return "none";
    const char* e = g_active->engine_name();
    return e ? e : "none";
}

// Diagnostik format roadmap §Phase 15 (TTY + serial).
void ghal_diag_dump(void) {
    extern void kprint(const char* s);
    extern void serial_print(const char* s);
    const char* be = ghal_active_backend_name();
    int acc = ghal_acceleration_enabled();
    const char* eng = ghal_engine_name();
    kprint("GPU:\n  backend: ");
    kprint(be);
    kprint(acc ? "\n  acceleration: enabled\n  engine: "
               : "\n  acceleration: disabled\n  engine: ");
    kprint(eng);
    kprint("\n");
    serial_print("GPU:\n  backend: ");
    serial_print(be);
    serial_print(acc ? "\n  acceleration: enabled\n  engine: "
                     : "\n  acceleration: disabled\n  engine: ");
    serial_print(eng);
    serial_print("\n");
}

// --- framebuffer & scanout size ---
extern void software_backend_set_fb(uint32_t* fb, uint32_t w, uint32_t h, uint32_t pitch_bytes);
extern void intel_backend_set_fb(uint32_t* fb, uint32_t w, uint32_t h, uint32_t pitch_bytes);

void ghal_set_framebuffer(uint32_t* fb, uint32_t width, uint32_t height,
                          uint32_t pitch_bytes) {
    software_backend_set_fb(fb, width, height, pitch_bytes);
    intel_backend_set_fb(fb, width, height, pitch_bytes);
}

void ghal_scanout_size(uint32_t* w, uint32_t* h) {
    // Satu jalur: mode aktif backend. Tidak lagi dispatch terpisah per backend.
    display_mode_t m;
    if (ghal_mode_get(&m) == 0) {
        if (w) *w = m.width;
        if (h) *h = m.height;
        return;
    }
    if (w) *w = 0;
    if (h) *h = 0;
}

// --- Display mode API ---

int ghal_mode_get(display_mode_t* out) {
    if (!g_active || !out || !g_active->mode_get) return -1;
    return g_active->mode_get(out);
}

int ghal_mode_enumerate(display_mode_t* out, uint32_t max) {
    if (!g_active || !out || max == 0 || !g_active->mode_enumerate) return -1;
    return g_active->mode_enumerate(out, max);
}

int ghal_mode_can_set(void) {
    return (g_active && (g_active->capabilities & GHAL_CAP_MODE_SET) &&
            g_active->mode_set) ? 1 : 0;
}

int ghal_mode_set(const display_mode_t* mode) {
    if (!mode || !ghal_mode_can_set()) return -1;
    return g_active->mode_set(mode);
}

// ------------------------------------------------------------
// Dispatch ke backend aktif (NULL-safe).
// ------------------------------------------------------------

ghal_surface_t* ghal_surface_create(uint32_t w, uint32_t h, ghal_format_t fmt) {
    if (!g_active) { g_last_error = "no active backend"; return NULL; }
    return g_active->surface_create(w, h, fmt);
}

ghal_surface_t* ghal_surface_create_scanout(uint32_t w, uint32_t h, ghal_format_t fmt) {
    if (!g_active) { g_last_error = "no active backend"; return NULL; }
    if (g_active->surface_create_scanout)
        return g_active->surface_create_scanout(w, h, fmt);
    return g_active->surface_create(w, h, fmt);
}

void ghal_surface_destroy(ghal_surface_t* s) {
    if (!g_active || !s) return;
    g_active->surface_destroy(s);
}

void ghal_surface_upload(ghal_surface_t* s, const uint32_t* src,
                         uint32_t src_pitch, ghal_rect_t rect) {
    if (!g_active || !s || !src) return;
    g_active->surface_upload(s, src, src_pitch, rect);
}

void ghal_fill_rect(ghal_surface_t* dst, ghal_rect_t rect, uint32_t argb) {
    if (!g_active || !dst) return;
    g_active->fill_rect(dst, rect, argb);
}

void ghal_blit(ghal_surface_t* dst, ghal_rect_t dst_rect,
               ghal_surface_t* src, ghal_rect_t src_rect) {
    if (!g_active || !dst || !src) return;
    g_active->blit(dst, dst_rect, src, src_rect);
}

void ghal_present(ghal_surface_t* s, const ghal_rect_t* rect) {
    if (!g_active || !s) return;
    g_active->present(s, rect);
}

// --- Fence async present (Phase 2C §9.2). Backend sync: fence selalu 0,
// jadi semua panggilan di bawah jatuh ke jalur no-op. ---

uint64_t ghal_present_fence(void) {
    if (!g_active || !g_active->present_fence) return 0;
    return g_active->present_fence();
}

int ghal_fence_pending(uint64_t fence) {
    if (!g_active || !g_active->fence_pending || fence == 0) return 0;
    return g_active->fence_pending(fence);
}

void ghal_fence_wait(uint64_t fence) {
    if (!g_active || !g_active->fence_wait || fence == 0) return;
    g_active->fence_wait(fence);
}

// --- Hardware cursor (Phase 2C §9.4) — fail-fast bila cap/ops tidak ada ---

int ghal_cursor_update(ghal_surface_t* cursor_img, int hot_x, int hot_y) {
    if (!g_active || !(g_active->capabilities & GHAL_CAP_HW_CURSOR) ||
        !g_active->cursor_update) return -1;
    return g_active->cursor_update(cursor_img, hot_x, hot_y);
}

void ghal_cursor_move(int x, int y) {
    if (!g_active || !(g_active->capabilities & GHAL_CAP_HW_CURSOR) ||
        !g_active->cursor_move) return;
    g_active->cursor_move(x, y);
}

// --- Statistik GPU (Phase 2C §9.6) ---

int ghal_gpu_stats(ghal_gpu_stats_t* out) {
    if (!g_active || !g_active->gpu_stats || !out) return -1;
    return g_active->gpu_stats(out);
}

// Dump statistik ke TTY (shell `gpu`). Format manual — freestanding.
void ghal_stats_dump(void) {
    extern void kprint(const char* s);
    extern void kprint_num(uint64_t v);
    if (!g_active) { kprint("[gpu] backend belum aktif\n"); return; }
    kprint("[gpu] backend=");
    kprint(g_active->name);
    ghal_gpu_stats_t st;
    if (ghal_gpu_stats(&st) != 0) { kprint(" (statistik tidak tersedia)\n"); return; }
    kprint("\n[gpu] present=");
    kprint_num(st.present_count);
    kprint("  cmd=");
    kprint_num(st.cmd_count);
    kprint("  bytes=");
    kprint_num(st.cmd_bytes);
    kprint("\n[gpu] notify=");
    kprint_num(st.notify_count);
    kprint("  wait_calls=");
    kprint_num(st.wait_calls);
    kprint("  wait_ticks=");
    kprint_num(st.wait_ticks);
    kprint("  err=");
    kprint_num(st.err_count);
    kprint("\n");
}
