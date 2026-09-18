#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stddef.h>

// ============================================================
// Display System — Phase 3 (Display System Rewrite)
//
// Generic, pixel-based buffer. Knows nothing about windows, text, or scroll.
// Higher layers (Line Buffer, Window Manager surfaces) build on top of this.
// ============================================================

// --- Locked design decisions (GUI_ROADMAP Phase 3 / "Design Decisions to Lock") ---
//
// Color format: XRGB8888. Low 24 bits = 0xRRGGBB. High byte = opacity mask:
//   0x00 = transparent, non-zero = opaque. This matches every existing layer
//   (base_canvas, backbuffer, window canvas, compositor) so no conversion ever
//   happens mid-pipeline — the roadmap flags mid-pipeline conversion as a bug source.
//
// Ownership: display_buffer_create() allocates pixels — the DisplayBuffer OWNS
//   them and frees them in display_buffer_destroy(). display_buffer_wrap() borrows
//   caller-owned memory — destroy() never frees it. Window Manager (Phase 5) uses
//   create() to request one owned buffer per window.

// ============================================================
// Display Mode — authoritative screen geometry (single source of truth)
//
// Semua konsumen kernel (compositor, KWM, mouse, TTY, panic, syscall 63)
// membaca mode dari sini, BUKAN dari global fb_* terpisah. Backend GHAL
// melaporkan mode-nya lewat mode_get/mode_enumerate; Display menyinkronkan
// mode aktif dari backend saat boot. Runtime set-mode adalah kapabilitas
// backend (software/Limine = boot-fixed → unsupported).
// ============================================================
typedef enum {
    DISPLAY_FMT_XRGB8888 = 0   // 32bpp, memori little-endian 0x00RRGGBB
} display_format_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch_bytes;   // byte per baris (>= width * bpp / 8)
    uint32_t bpp;
    uint32_t format;        // display_format_t
} display_mode_t;

// Deskripsi format framebuffer mentah (dari Limine) untuk validasi boot.
typedef struct {
    uint32_t bpp;
    uint32_t memory_model;   // LIMINE_FRAMEBUFFER_RGB = 1
    uint32_t red_size,   red_shift;
    uint32_t green_size, green_shift;
    uint32_t blue_size,  blue_shift;
} display_format_desc_t;

// Mode aktif (NULL sebelum display_boot_init). Pointer statis — aman dibaca
// dari konteks IRQ, tidak pernah di-free.
const display_mode_t* display_get_mode(void);

// Enumerasi mode yang didukung backend aktif. Return jumlah (>0) atau <0.
int display_get_modes(display_mode_t* out, uint32_t max);

// Minta ganti mode runtime. Task context saja. Return 0 sukses, <0 ditolak
// (backend boot-fixed / tanpa kapabilitas GHAL_CAP_MODE_SET).
int display_set_mode(const display_mode_t* mode);

// --- Framebuffer/buffer lifecycle (kernel/gfx/fb.c) ---
// Tangkap + validasi framebuffer Limine. Tidak mengalokasi (heap belum siap).
int display_boot_init(uint32_t* fb, uint32_t width, uint32_t height,
                      uint32_t pitch_bytes, const display_format_desc_t* fmt);

// Sinkronkan mode aktif dari backend GHAL aktif (virtio pmodes[0] dsb).
int display_sync_from_backend(void);

// Alokasikan base_canvas/backbuffer seukuran mode (pitch-aware, overflow-safe).
// Wajib task context (kmalloc). Idempotent.
int display_alloc_buffers(void);

typedef uint32_t Color;

typedef enum {
    COLOR_FORMAT_XRGB8888 = 0
} ColorFormat;

typedef struct {
    int32_t x, y;
    uint32_t width, height;
} Rect;

struct DirtyRegionList;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;       // pixels per row (>= width)
    ColorFormat format;
    uint32_t* pixels;
    uint8_t owns_pixels;   // 1 = destroy() frees pixels, 0 = borrowed
    // Kontrak Phase 3B: setiap write ke buffer OTOMATIS menandai dirty.
    // NULL = tanpa tracking. Marking TIDAK mengambil lock — pasang hanya
    // pada buffer yang disentuh satu konteks (buffer layar pakai jalur
    // terkunci screen_mark_dirty di fb.c, bukan field ini).
    struct DirtyRegionList* dirty;
} DisplayBuffer;

// Allocates pixels (System-owned). Returns NULL on failure.
DisplayBuffer* display_buffer_create(uint32_t width, uint32_t height, ColorFormat format);

// Borrows caller-owned pixels. stride is pixels per row. Returns NULL on failure.
DisplayBuffer* display_buffer_wrap(uint32_t* pixels, uint32_t width, uint32_t height,
                                   uint32_t stride, ColorFormat format);

void display_buffer_destroy(DisplayBuffer* buffer);

void display_buffer_write_pixel(DisplayBuffer* buffer, int32_t x, int32_t y, Color color);
void display_buffer_fill_rect(DisplayBuffer* buffer, Rect area, Color color);

// --- Rect helpers ---
// Intersection of a and b written to *out. Returns 0 when empty (out untouched).
int rect_intersect(Rect a, Rect b, Rect* out);
// Smallest rect containing both a and b.
Rect rect_union(Rect a, Rect b);

// ============================================================
// Dirty region tracking (Phase 3B; coalescing Phase 15)
//
// Only touched screen areas are recomposited/presented each frame. Marks are
// coalesced on insert: contained/overlapping rects merge in place, and when the
// list is full the cheapest pair is merged (area-inflation heuristic) instead
// of collapsing everything into one bounding box. The coalesced set is always a
// correct SUPERSET of what changed — never less (over-invalidation is safe;
// under-invalidation is not).
// ============================================================

#define MAX_DIRTY_REGIONS 64

typedef struct DirtyRegionList {
    Rect regions[MAX_DIRTY_REGIONS];   // live region count == `count`
    uint32_t count;
    uint8_t collapsed;   // retained for layout compatibility; unused since Phase 15
} DirtyRegionList;

void dirty_region_clear(DirtyRegionList* list);
void dirty_region_mark(DirtyRegionList* list, Rect r);

// ============================================================
// Viewport (Phase 3C)
//
// A scrollable window onto a source DisplayBuffer. Knows only how to cut and
// shift a region of pixels — nothing about text or history. Phase 5 (Window
// Manager) reuses this to clip window surfaces onto the screen.
// ============================================================

typedef struct {
    Rect bounds;          // where on the target the viewport is drawn
    int32_t scroll_x;
    int32_t scroll_y;
    DisplayBuffer* source;
} Viewport;

void viewport_scroll(Viewport* vp, int32_t dx, int32_t dy);
// Blit the scrolled region of vp->source into target at vp->bounds.
void viewport_render(Viewport* vp, DisplayBuffer* target);

#endif
