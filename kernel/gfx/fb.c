#include <stdint.h>
#include <stddef.h>
#define FONT8x16_IMPLEMENTATION
#include "font8x16.h"
#include "gfx.h"
#include "display.h"
#include "ghal.h"
#include "heap.h"
#include "string.h"
#include "limine.h"

// ============================================================
// Framebuffer + Display Mode (kernel/gfx/fb.c)
//
// Satu sumber kebenaran geometri layar: `g_mode`. Semua konsumen kernel
// membaca display_get_mode(); tidak ada lagi global fb_width/fb_height/fb_pitch.
// base_canvas/backbuffer dialokasikan seukuran mode (pitch-aware), bukan lagi
// array statis 1920x1080 (bisa overflow pada mode besar / pitch berpadding).
// ============================================================

// Batas sane dimensi — seragam dengan GHAL_MAX_DIM (graphics/memory/gpu_alloc.h)
// dan tidak melebihi KWM_MAX_DIMENSION per-window.
#define DISPLAY_MAX_DIM 8192U

// --- Framebuffer hardware (dialokasikan firmware; tidak pernah di-free) ---
uint32_t* fb_ptr = NULL;

// --- Screen buffers (dialokasikan display_alloc_buffers) ---
uint32_t* backbuffer  = NULL;
uint32_t* base_canvas = NULL;

// --- Mode aktif (authoritative) ---
static display_mode_t g_mode;
static int g_mode_valid = 0;
static int g_buffers_ready = 0;

// DisplayBuffer statis yang membungkus base_canvas/backbuffer/fb_ptr (borrow).
static DisplayBuffer g_screen_db;   // base_canvas
static DisplayBuffer g_back_db;     // backbuffer
static DisplayBuffer g_fb_db;       // framebuffer hardware

static void wrap_db(DisplayBuffer* db, uint32_t* pixels) {
    db->pixels      = pixels;
    db->width       = g_mode.width;
    db->height      = g_mode.height;
    db->stride      = g_mode.pitch_bytes / 4;
    db->format      = COLOR_FORMAT_XRGB8888;
    db->owns_pixels = 0;
    db->dirty       = NULL;
}

// ------------------------------------------------------------
// Mode API
// ------------------------------------------------------------

const display_mode_t* display_get_mode(void) {
    return g_mode_valid ? &g_mode : NULL;
}

int display_get_modes(display_mode_t* out, uint32_t max) {
    if (!out || max == 0) return -1;
    int n = ghal_mode_enumerate(out, max);
    if (n > 0) return n;
    if (g_mode_valid) { out[0] = g_mode; return 1; }   // backend belum aktif
    return -1;
}

int display_set_mode(const display_mode_t* mode) {
    if (!mode || !g_mode_valid) return -1;
    // Runtime switch = kapabilitas backend. Tidak ada teardown/realloc di sini:
    // software/Limine boot-fixed → ghal_mode_set() menolak, state lama utuh.
    // Backend yang benar-benar mendukung harus membangun resource baru lebih
    // dulu (task context) sebelum resource lama dilepas.
    return ghal_mode_set(mode);
}

// ------------------------------------------------------------
// Boot: tangkap + validasi framebuffer Limine. Tidak mengalokasi (heap belum
// siap) — hanya menetapkan mode. Halt-on-invalid ditangani pemanggil.
// ------------------------------------------------------------
int display_boot_init(uint32_t* fb, uint32_t width, uint32_t height,
                      uint32_t pitch_bytes, const display_format_desc_t* fmt) {
    if (!fb || !fmt) return -1;
    if (width == 0 || height == 0) return -1;
    if (width > DISPLAY_MAX_DIM || height > DISPLAY_MAX_DIM) return -1;

    // pitch minimal width*4 dan tidak overflow.
    uint64_t min_pitch = (uint64_t)width * 4ULL;
    if (pitch_bytes == 0 || (uint64_t)pitch_bytes < min_pitch) return -1;
    if ((uint64_t)pitch_bytes > (uint64_t)SIZE_MAX) return -1;

    // Format: 32bpp RGB, mask 8/8/8, layout XRGB8888 (R di bit 16, B di bit 0).
    // Bukan asumsi diam-diam lagi — mode/format lain ditolak eksplisit.
    if (fmt->bpp != 32) return -1;
    if (fmt->memory_model != LIMINE_FRAMEBUFFER_RGB) return -1;
    if (fmt->red_size != 8 || fmt->green_size != 8 || fmt->blue_size != 8) return -1;
    if (fmt->red_shift != 16 || fmt->green_shift != 8 || fmt->blue_shift != 0) return -1;

    fb_ptr = fb;
    g_mode.width       = width;
    g_mode.height      = height;
    g_mode.pitch_bytes = pitch_bytes;
    g_mode.bpp         = fmt->bpp;
    g_mode.format      = DISPLAY_FMT_XRGB8888;
    g_mode_valid       = 1;
    return 0;
}

// Sinkronkan mode aktif dari backend GHAL (mis. pmodes[0] virtio-gpu).
// Software backend mengembalikan ukuran framebuffer Limine yang sama.
int display_sync_from_backend(void) {
    display_mode_t m;
    if (ghal_mode_get(&m) != 0) return -1;
    if (m.width == 0 || m.height == 0) return -1;
    if (m.width > DISPLAY_MAX_DIM || m.height > DISPLAY_MAX_DIM) return -1;
    if (m.pitch_bytes == 0 ||
        (uint64_t)m.pitch_bytes < (uint64_t)m.width * 4ULL) return -1;

    g_mode = m;
    g_mode_valid = 1;
    return 0;
}

// ------------------------------------------------------------
// Alokasi buffer layar seukuran mode. Task context (kmalloc). Idempotent.
// Ukuran = pitch_bytes * height (BUKAN width*height) — pitch berpadding aman.
// ------------------------------------------------------------
int display_alloc_buffers(void) {
    if (g_buffers_ready) return 0;
    if (!g_mode_valid) return -1;

    uint64_t stride_bytes = g_mode.pitch_bytes;
    uint64_t bytes = stride_bytes * (uint64_t)g_mode.height;
    if (stride_bytes == 0 || bytes / stride_bytes != (uint64_t)g_mode.height)
        return -1;   // overflow
    if (bytes > (uint64_t)SIZE_MAX) return -1;

    base_canvas = (uint32_t*)kmalloc((size_t)bytes);
    backbuffer  = (uint32_t*)kmalloc((size_t)bytes);
    if (!base_canvas || !backbuffer) {
        if (base_canvas) { kfree(base_canvas); base_canvas = NULL; }
        if (backbuffer)  { kfree(backbuffer);  backbuffer  = NULL; }
        return -1;
    }
    memset(base_canvas, 0, (size_t)bytes);
    memset(backbuffer,  0, (size_t)bytes);

    wrap_db(&g_screen_db, base_canvas);
    wrap_db(&g_back_db,   backbuffer);
    wrap_db(&g_fb_db,     fb_ptr);
    g_buffers_ready = 1;
    return 0;
}

static inline int gfx_buffers_ready(void) { return g_buffers_ready; }

DisplayBuffer* gfx_screen_buffer(void) { return gfx_buffers_ready() ? &g_screen_db : NULL; }
DisplayBuffer* gfx_back_buffer(void)   { return gfx_buffers_ready() ? &g_back_db   : NULL; }
DisplayBuffer* gfx_fb_buffer(void)     { return gfx_buffers_ready() ? &g_fb_db     : NULL; }

// Tulis satu pixel TANPA dirty-mark — dipakai loop internal yang menandai
// satu rect utuh di akhir (mark per-pixel = badai spinlock).
static inline void screen_put(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= g_screen_db.width || y >= g_screen_db.height) return;
    g_screen_db.pixels[y * g_screen_db.stride + x] = color;
}

// Kontrak Phase 3B: SEMUA primitive di bawah menandai dirty sendiri —
// caller tidak perlu (dan tidak boleh perlu) memanggil screen_mark_dirty.

void draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!gfx_buffers_ready()) return;
    screen_put(x, y, color);
    screen_mark_dirty((int32_t)x, (int32_t)y, 1, 1);
}

void draw_rect(uint32_t start_x, uint32_t start_y, uint32_t width, uint32_t height, uint32_t color) {
    if (!gfx_buffers_ready()) return;
    Rect r = { (int32_t)start_x, (int32_t)start_y, width, height };
    display_buffer_fill_rect(&g_screen_db, r, color);
    screen_mark_dirty((int32_t)start_x, (int32_t)start_y, width, height);
}

void draw_image(int start_x, int start_y, int width, int height, uint32_t* buffer) {
    if (!gfx_buffers_ready()) return;
    int i = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t pixel = buffer[i++];
            uint8_t alpha = (pixel >> 24) & 0xFF;
            if (alpha > 0) {
                screen_put((uint32_t)(start_x + x), (uint32_t)(start_y + y), pixel & 0xFFFFFF);
            }
        }
    }
    screen_mark_dirty(start_x, start_y, (uint32_t)width, (uint32_t)height);
}

void draw_char(char c, uint32_t x, uint32_t y, uint32_t color) {
    if (c < 0 || c > 127) return;
    if (!gfx_buffers_ready()) return;
    const unsigned char* bitmap = font8x16[(int)c];
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 8; col++) {
            if (bitmap[row] & (0x80 >> col)) screen_put(x + col, y + row, color);
        }
    }
    screen_mark_dirty((int32_t)x, (int32_t)y, 8, 16);
}

void draw_string(const char* str, uint32_t x, uint32_t y, uint32_t color) {
    uint32_t curr_x = x;
    uint32_t curr_y = y;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') { curr_y += 16; curr_x = x; }
        else { draw_char(str[i], curr_x, curr_y, color); curr_x += 8; }
    }
}
