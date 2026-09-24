// ============================================================
// Software GPU Backend (graphics/backend/software.c)
//
// Backend fallback murni CPU: ghal_surface_t = buffer system RAM
// (XRGB8888). Semua operasi = memcpy/loop langsung ke pixels.
//
// ghal_surface_t adalah OPAQUE dari sisi HAL; definisi konkret ada
// di sini. Backend ini tidak pernah gagal init (murni RAM).
// ============================================================

#include "ghal.h"
#include "heap.h"
#include <string.h>
#include <stddef.h>   // NULL

// Struct konkret surface untuk backend ini (tidak diekspos ke HAL).
struct ghal_surface {
    uint32_t    width;
    uint32_t    height;
    uint32_t    stride;      // piksel per baris (framebuffer bisa != width)
    ghal_format_t format;
    uint32_t*   pixels;
    uint8_t     owns_pixels; // 0 = membungkus framebuffer HW (jangan di-free)
};

// Framebuffer hardware (di-set lewat ghal_set_framebuffer sebelum init).
static uint32_t* g_fb;
static uint32_t  g_fb_w, g_fb_h, g_fb_pitch4;

void software_backend_set_fb(uint32_t* fb, uint32_t w, uint32_t h, uint32_t pitch_bytes) {
    g_fb = fb; g_fb_w = w; g_fb_h = h; g_fb_pitch4 = pitch_bytes / 4;
}

static struct ghal_surface* sw_surface_create(uint32_t w, uint32_t h, ghal_format_t fmt) {
    if (w == 0 || h == 0) return NULL;
    // Cek overflow width*height*4 (pola audit 5.6).
    uint64_t bytes = (uint64_t)w * h * 4;
    if (w != 0 && (bytes / 4 / w) != h) return NULL;

    struct ghal_surface* s = (struct ghal_surface*)kmalloc(sizeof(*s));
    if (!s) return NULL;
    s->pixels = (uint32_t*)kmalloc((size_t)bytes);
    if (!s->pixels) { kfree(s); return NULL; }
    memset(s->pixels, 0, (size_t)bytes);
    s->width = w;
    s->height = h;
    s->stride = w;
    s->format = fmt;
    s->owns_pixels = 1;
    return s;
}

// Scanout surface: BUNGKUS framebuffer hardware langsung. Upload menulis
// langsung ke layar, present jadi no-op — menghilangkan double-copy
// (backbuffer → surface → framebuffer) yang membuat present 2x lebih mahal.
static struct ghal_surface* sw_surface_create_scanout(uint32_t w, uint32_t h,
                                                     ghal_format_t fmt) {
    if (g_fb == NULL) return sw_surface_create(w, h, fmt);
    struct ghal_surface* s = (struct ghal_surface*)kmalloc(sizeof(*s));
    if (!s) return NULL;
    s->pixels = g_fb;
    s->width  = w < g_fb_w ? w : g_fb_w;
    s->height = h < g_fb_h ? h : g_fb_h;
    s->stride = g_fb_pitch4;
    s->format = fmt;
    s->owns_pixels = 0;   // framebuffer HW — jangan di-free
    return s;
}

static void sw_surface_destroy(ghal_surface_t* s) {
    if (!s) return;
    if (s->owns_pixels && s->pixels) kfree(s->pixels);
    kfree(s);
}

static void sw_surface_upload(ghal_surface_t* s, const uint32_t* src,
                              uint32_t src_pitch, ghal_rect_t rect) {
    if (!s || !src) return;
    if (rect.x >= src_pitch) return;
    // Clamp ke surface.
    if (rect.x >= s->width || rect.y >= s->height) return;
    if (rect.w == 0 || rect.h == 0) return;
    uint32_t maxw = s->width - rect.x;
    uint32_t maxh = s->height - rect.y;
    if (maxw > src_pitch - rect.x) maxw = src_pitch - rect.x;
    if (rect.w > maxw) rect.w = maxw;
    if (rect.h > maxh) rect.h = maxh;

    const uint32_t* src_row = src + (uint64_t)rect.y * src_pitch + rect.x;
    uint32_t* dst_row = s->pixels + (uint64_t)rect.y * s->stride + rect.x;
    for (uint32_t y = 0; y < rect.h; y++) {
        memcpy(dst_row, src_row, rect.w * 4);
        src_row += src_pitch;
        dst_row += s->stride;
    }
}

static void sw_fill_rect(ghal_surface_t* dst, ghal_rect_t rect, uint32_t argb) {
    if (!dst) return;
    if (rect.x >= dst->width || rect.y >= dst->height) return;
    uint32_t maxw = dst->width - rect.x;
    uint32_t maxh = dst->height - rect.y;
    if (rect.w > maxw) rect.w = maxw;
    if (rect.h > maxh) rect.h = maxh;
    uint32_t color = argb & 0xFFFFFF;   // XRGB: top byte = alpha mask
    for (uint32_t y = 0; y < rect.h; y++) {
        uint32_t* row = dst->pixels + (uint64_t)(rect.y + y) * dst->stride + rect.x;
        for (uint32_t x = 0; x < rect.w; x++) row[x] = color;
    }
}

static void sw_blit(ghal_surface_t* dst, ghal_rect_t dst_rect,
                    ghal_surface_t* src, ghal_rect_t src_rect) {
    if (!dst || !src) return;
    // v1: scaling tidak didukung (roadmap §8.6) — dst ukuran harus == src.
    if (dst_rect.w != src_rect.w || dst_rect.h != src_rect.h) return;

    // Clamp dst.
    if (dst_rect.x >= dst->width || dst_rect.y >= dst->height) return;
    uint32_t maxw = dst->width - dst_rect.x;
    uint32_t maxh = dst->height - dst_rect.y;
    if (dst_rect.w > maxw) dst_rect.w = maxw;
    if (dst_rect.h > maxh) dst_rect.h = maxh;

    // Clamp src rect terhadap bounds source.
    if (src_rect.x >= src->width || src_rect.y >= src->height) return;
    uint32_t smaxw = src->width - src_rect.x;
    uint32_t smaxh = src->height - src_rect.y;
    if (src_rect.w > smaxw) src_rect.w = smaxw;
    if (src_rect.h > smaxh) src_rect.h = smaxh;

    // Ambil ukuran terkecil (dst & src sudah di-clamp).
    uint32_t w = dst_rect.w < src_rect.w ? dst_rect.w : src_rect.w;
    uint32_t h = dst_rect.h < src_rect.h ? dst_rect.h : src_rect.h;

    const uint32_t* srow = src->pixels + (uint64_t)src_rect.y * src->stride + src_rect.x;
    uint32_t* drow = dst->pixels + (uint64_t)dst_rect.y * dst->stride + dst_rect.x;
    for (uint32_t y = 0; y < h; y++) {
        memcpy(drow, srow, w * 4);
        srow += src->stride;
        drow += dst->stride;
    }
}

static void sw_present(ghal_surface_t* s, const ghal_rect_t* rect) {
    // Scanout surface (owns_pixels==0) SUDAH menunjuk framebuffer HW —
    // upload menulis langsung ke layar, jadi present tidak perlu copy apa pun.
    if (!s || !s->pixels || !g_fb) return;
    if (!s->owns_pixels) return;   // zero-copy path

    // Surface privat: salin region damage ke framebuffer hardware.
    ghal_rect_t r;
    if (rect) r = *rect;
    else { r.x = 0; r.y = 0; r.w = s->width; r.h = s->height; }

    if (r.x >= s->width || r.y >= s->height) return;
    if (r.x >= g_fb_w || r.y >= g_fb_h) return;
    uint32_t maxw = s->width - r.x, maxh = s->height - r.y;
    if (r.w > maxw) r.w = maxw;
    if (r.h > maxh) r.h = maxh;
    maxw = g_fb_w - r.x; maxh = g_fb_h - r.y;
    if (r.w > maxw) r.w = maxw;
    if (r.h > maxh) r.h = maxh;

    for (uint32_t y = 0; y < r.h; y++) {
        const uint32_t* srow = s->pixels + (uint64_t)(r.y + y) * s->stride + r.x;
        uint32_t* drow = g_fb + (uint64_t)(r.y + y) * g_fb_pitch4 + r.x;
        memcpy(drow, srow, r.w * 4);
    }
}

static int software_init(void) { return 0; }
static void software_shutdown(void) {}

// --- Display mode (boot-fixed: framebuffer Limine adalah scanout fisik) ---
// Software backend tidak bisa mengubah mode hardware; mode_set selalu ditolak
// (tanpa GHAL_CAP_MODE_SET). State lama selalu utuh.
static int software_mode_get(display_mode_t* out) {
    if (!out || g_fb_w == 0 || g_fb_h == 0) return -1;
    out->width       = g_fb_w;
    out->height      = g_fb_h;
    out->pitch_bytes = g_fb_pitch4 * 4;
    out->bpp         = 32;
    out->format      = DISPLAY_FMT_XRGB8888;
    return 0;
}

static int software_mode_enumerate(display_mode_t* out, uint32_t max) {
    if (!out || max == 0) return -1;
    if (software_mode_get(&out[0]) != 0) return -1;
    return 1;   // satu mode: mode framebuffer yang dinegosiasikan Limine
}

static int software_mode_set(const display_mode_t* mode) { (void)mode; return -1; }

const ghal_backend_ops_t software_backend_ops = {
    .name           = "software",
    .capabilities   = GHAL_CAP_PARTIAL_FLUSH,
    .init           = software_init,
    .shutdown       = software_shutdown,
    .surface_create = sw_surface_create,
    .surface_destroy= sw_surface_destroy,
    .surface_create_scanout = sw_surface_create_scanout,
    .surface_upload = sw_surface_upload,
    .fill_rect      = sw_fill_rect,
    .blit           = sw_blit,
    .present        = sw_present,
    .cursor_update  = NULL,
    .cursor_move    = NULL,
    .mode_get       = software_mode_get,
    .mode_enumerate = software_mode_enumerate,
    .mode_set       = software_mode_set,
    .mode_changed   = NULL,
};
