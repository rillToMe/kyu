#include <stdint.h>
#include "gfx.h"
#include "kwm_internal.h"
#include "display.h"
#include "spinlock.h"
#include "aa_math.h"
#include "heap.h"     // kmalloc/kfree — buffer image kursor HW (§9.4)
#include "ghal.h"     // Phase 2B: present lewat Graphics HAL

extern int32_t mouse_x;
extern int32_t mouse_y;
extern const uint8_t cursor_bitmap[16][12];
// Font VGA 8x16 (didefinisikan di kernel/gfx/fb.c) — teks judul titlebar.
extern const unsigned char font8x16[256][16];

#define CURSOR_WIDTH  12
#define CURSOR_HEIGHT 16

// Phase 9 — bentuk kursor tambahan (0 = tembus, 1 = putih, 2 = hitam).
// Ukuran 12x16 sama dengan panah → logika dirty-rect & clamp mouse_x/y
// (fb-12/fb-16) tidak berubah. Hot point tetap (0,0) untuk semua bentuk.
static const uint8_t g_ibeam_bitmap[16][12] = {
    {0,1,1,1,1,1,1,1,1,1,1,0},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {0,1,1,1,1,1,1,1,1,1,1,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,0},
    {1,2,2,2,2,2,2,2,2,2,2,1},
};

static const uint8_t g_hand_bitmap[16][12] = {
    {0,0,0,0,0,0,0,1,1,0,0,0},
    {0,0,0,0,0,1,1,2,1,0,0,0},
    {0,0,0,0,1,1,2,2,1,0,0,0},
    {0,0,0,1,1,2,2,2,1,0,0,0},
    {0,0,0,1,2,2,2,2,1,0,0,0},
    {0,0,0,1,2,2,2,2,2,1,0,0},
    {0,0,1,1,2,2,2,2,2,1,0,0},
    {0,1,1,2,2,2,2,2,2,1,0,0},
    {0,1,2,2,2,2,2,2,2,1,0,0},
    {1,1,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {0,1,2,2,2,2,2,2,2,1,0,0},
    {0,1,1,2,2,2,2,2,1,1,0,0},
    {0,0,1,1,1,1,1,1,1,0,0,0},
};

static int g_cursor_kind = 0;

// Dirty-region state (Phase 3B). Every draw into base_canvas and every window
// move marks the touched rect here; compositor_flush recomposites and presents
// only these rects instead of the whole screen. Idle frames are near no-ops.
static DirtyRegionList g_screen_dirty;
static spinlock_t g_dirty_lock = SPINLOCK_INIT;
static int32_t g_last_cursor_x = -1;
static int32_t g_last_cursor_y = -1;

void screen_mark_dirty(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    Rect r = { x, y, width, height };
    uint64_t flags = spinlock_lock_irqsave(&g_dirty_lock);
    dirty_region_mark(&g_screen_dirty, r);
    spinlock_unlock_irqrestore(&g_dirty_lock, flags);
}

// Copy a screen-space rect between two full-screen buffers.
// Phase 3C ADOPSI: blit = viewport_render dengan scroll = posisi rect —
// Viewport adalah mesin blit standar compositor (dipakai lagi oleh WM
// untuk surface di Phase 5).
static void blit_rect_db(DisplayBuffer* dst, DisplayBuffer* src, Rect r) {
    Viewport vp = { r, r.x, r.y, src };
    viewport_render(&vp, dst);
}

// Isi area solid, dipotong ke `clip` (bounds check per baris sekali).
static void fill_rect_clip(uint32_t* fb, int pitch4, Rect area, uint32_t color,
                           Rect clip) {
    Rect c;
    if (!rect_intersect(area, clip, &c)) return;
    for (uint32_t yy = 0; yy < c.height; yy++) {
        uint32_t* dst = fb + ((uint32_t)c.y + yy) * (uint32_t)pitch4 + (uint32_t)c.x;
        for (uint32_t xx = 0; xx < c.width; xx++) dst[xx] = color;
    }
}

// --- Primitif modern: blend / gradient / rounded ---
// Math (mix/shade/coverage) di include/aa_math.h — dibagi dengan toolkit
// userspace apps/libui.cpp, integer saja (tanpa SSE/float).
static inline void blend_px(uint32_t* fb, int pitch4, int x, int y,
                            uint32_t c, uint32_t a, Rect clip) {
    if (a == 0) return;
    if (x < clip.x || x >= clip.x + (int)clip.width ||
        y < clip.y || y >= clip.y + (int)clip.height) return;
    uint32_t* d = &fb[y * pitch4 + x];
    *d = aa_mix(c, *d, a);
}

static void blend_rect_clip(uint32_t* fb, int pitch4, Rect area, uint32_t c,
                            uint32_t a, Rect clip) {
    Rect k;
    if (a == 0 || !rect_intersect(area, clip, &k)) return;
    for (uint32_t yy = 0; yy < k.height; yy++) {
        uint32_t* dst = fb + ((uint32_t)k.y + yy) * (uint32_t)pitch4 + (uint32_t)k.x;
        for (uint32_t xx = 0; xx < k.width; xx++) dst[xx] = aa_mix(c, dst[xx], a);
    }
}

static inline uint32_t rr_cov(int px, int py, Rect a, int rt, int rb) {
    return aa_cov(px, py, a.x, a.y, (int)a.width, (int)a.height, rt, rb);
}

// Fill rounded-rect + gradient vertikal (top→bot), sudut anti-alias, dipotong
// ke `clip`. rt/rb = radius sudut atas/bawah (0 = kotak).
static void round_grad_fill(uint32_t* fb, int pitch4, Rect a,
                            uint32_t top, uint32_t bot, int rt, int rb, Rect clip) {
    int W = (int)a.width, H = (int)a.height;
    if (W <= 0 || H <= 0) return;
    if (rt > W / 2) rt = W / 2;
    if (rb > W / 2) rb = W / 2;
    for (int iy = a.y; iy < a.y + H; iy++) {
        uint32_t c = (top == bot) ? top
                   : aa_mix(bot, top, (uint32_t)((iy - a.y) * 255 / (H > 1 ? H - 1 : 1)));
        int r = (rt && iy < a.y + rt) ? rt
              : (rb && iy >= a.y + H - rb) ? rb : 0;
        if (!r) {
            Rect row = { a.x, iy, a.width, 1 };
            fill_rect_clip(fb, pitch4, row, c, clip);
            continue;
        }
        Rect mid = { a.x + r, iy, (uint32_t)(W - 2 * r), 1 };
        fill_rect_clip(fb, pitch4, mid, c, clip);
        for (int k = 0; k < r; k++) {
            blend_px(fb, pitch4, a.x + k, iy, c, rr_cov(a.x + k, iy, a, rt, rb), clip);
            blend_px(fb, pitch4, a.x + W - 1 - k, iy, c,
                     rr_cov(a.x + W - 1 - k, iy, a, rt, rb), clip);
        }
    }
}

// Drop shadow frame window: KWM_SHADOW_MARGIN ring 1px alpha menurun, offset
// 3px ke bawah (blur semu). Digambar SEBELUM konten+titlebar window itu, jadi
// ring yang jatuh di dalam frame ditimpa lagi.
// ponytail: ring 1px, bukan gaussian; dirty-rect frame sudah diperlebar
// KWM_SHADOW_MARGIN di kwm.c — naikkan keduanya bersama bila shadow diperbesar.
static void frame_shadow(uint32_t* fb, int pitch4, Rect frame, Rect clip) {
    static const uint32_t alpha[KWM_SHADOW_MARGIN] = { 58, 46, 34, 24, 15, 8 };
    for (int k = 1; k <= KWM_SHADOW_MARGIN; k++) {
        Rect s = { frame.x - k, frame.y - k + 3,
                   frame.width + 2 * (uint32_t)k, frame.height + 2 * (uint32_t)k };
        uint32_t a = alpha[k - 1];
        Rect t = { s.x, s.y, s.width, 1 };
        Rect b = { s.x, s.y + (int)s.height - 1, s.width, 1 };
        Rect l = { s.x, s.y + 1, 1, s.height - 2 };
        Rect rr = { s.x + (int)s.width - 1, s.y + 1, 1, s.height - 2 };
        blend_rect_clip(fb, pitch4, t, 0x000000, a, clip);
        blend_rect_clip(fb, pitch4, b, 0x000000, a, clip);
        blend_rect_clip(fb, pitch4, l, 0x000000, a, clip);
        blend_rect_clip(fb, pitch4, rr, 0x000000, a, clip);
    }
}

// Segmen garis (DDA) dipotong ke `clip` — untuk glyph "X" tombol close.
static void titlebar_line(uint32_t* fb, int pitch4,
                          int x0, int y0, int x1, int y1,
                          uint32_t color, Rect clip) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (dx * i) / steps;
        int y = y0 + (dy * i) / steps;
        if (x >= clip.x && x < clip.x + (int)clip.width &&
            y >= clip.y && y < clip.y + (int)clip.height)
            fb[y * pitch4 + x] = color;
    }
}

// Teks judul window di titlebar (Phase 10). Digambar karakter per karakter
// (font 8x16), clamp ke `max_x` (area sebelum tombol close) + clip rect.
static void titlebar_text(uint32_t* fb, int pitch4, int x, int y,
                          const char* str, int max_x, uint32_t color, Rect clip) {
    for (int i = 0; str[i] && x + 8 <= max_x; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c > 127) { x += 8; continue; }
        const unsigned char* bmp = font8x16[c];
        for (int row = 0; row < 16; row++) {
            int py = y + row;
            if (py < (int)clip.y || py >= (int)(clip.y + clip.height)) continue;
            for (int col = 0; col < 8; col++) {
                if (!(bmp[row] & (0x80 >> col))) continue;
                int px = x + col;
                if (px < (int)clip.x || px >= (int)(clip.x + clip.width)) continue;
                fb[py * pitch4 + px] = color;
            }
        }
        x += 8;
    }
}

// Composite every window overlapping `r` (z-order low→high) onto the backbuffer.
// Caller must hold kwm_lock. Phase 5C: frame window = konten + titlebar milik
// WM; titlebar digambar di sini (tint beda untuk window fokus + tombol close).
static void composite_windows_in_rect(Rect r, int pitch4) {
    // Phase 10: z=0 adalah window desktop (paling bawah).
    for (uint32_t z = 0; z <= next_z_index; z++) {
        for (int w = 0; w < MAX_WINDOWS; w++) {
            if (!(kwm_windows[w].active && kwm_windows[w].z_index == z && kwm_windows[w].canvas))
                continue;

            const int32_t win_x = kwm_windows[w].x;
            const int32_t win_y = kwm_windows[w].y;              // frame atas
            // Phase 10: desktop frameless — konten mulai di y window.
            const uint32_t wflags = kwm_windows[w].flags;
            const uint32_t tb_h = (wflags & KWM_WIN_DESKTOP) ? 0 : KWM_TITLEBAR_H;
            const int32_t cty    = win_y + (int32_t)tb_h;        // konten mulai
            const uint32_t cw    = kwm_windows[w].width;
            const uint32_t ch    = kwm_windows[w].height;
            const DisplayBuffer* canvas = kwm_windows[w].canvas;

            // --- Drop shadow frame (kecuali desktop) ---
            if (!(wflags & KWM_WIN_DESKTOP)) {
                Rect frame = { win_x, win_y, cw, ch + tb_h };
                frame_shadow(backbuffer, pitch4, frame, r);
            }

            // --- Konten (canvas = konten murni) ---
            Rect crect = { win_x, cty, cw, ch };
            Rect clip;
            if (rect_intersect(crect, r, &clip)) {
                for (uint32_t yy = 0; yy < clip.height; yy++) {
                    int32_t sy = clip.y + (int32_t)yy - cty;
                    const uint32_t* src = canvas->pixels +
                        (uint32_t)sy * canvas->stride + (uint32_t)(clip.x - win_x);
                    uint32_t* dst = backbuffer +
                        ((uint32_t)clip.y + yy) * (uint32_t)pitch4 + (uint32_t)clip.x;
                    for (uint32_t xx = 0; xx < clip.width; xx++) {
                        uint32_t pixel = src[xx];
                        // Alpha byte acts as a per-pixel mask: 0 = transparent.
                        if (pixel >> 24) dst[xx] = pixel & 0xFFFFFF;
                    }
                }
            }

            // --- Titlebar milik WM (kecuali desktop: frameless) ---
            if (!(wflags & KWM_WIN_DESKTOP)) {
                Rect tb = { win_x, win_y, cw, KWM_TITLEBAR_H };
                if (rect_intersect(tb, r, &clip)) {
                    // Gradient vertikal ~±12% + sudut atas membulat (AA).
                    uint32_t base = (w == focused_win_id) ? KWM_TITLEBAR_COLOR
                                                          : KWM_TITLEBAR_INACT;
                    round_grad_fill(backbuffer, pitch4, tb,
                                    aa_shade(base, 14), aa_shade(base, -12),
                                    KWM_CORNER_R, 0, r);
                    // Tombol close: rounded square di dalam area klik 40px.
                    Rect cb = { win_x + (int32_t)cw - KWM_CLOSE_BTN_W / 2 - 11,
                                win_y + 3, 22, 18 };
                    round_grad_fill(backbuffer, pitch4, cb,
                                    aa_shade(KWM_CLOSE_COLOR, 12),
                                    aa_shade(KWM_CLOSE_COLOR, -10), 4, 4, r);
                    // Glyph "X" di dalam tombol close.
                    titlebar_line(backbuffer, pitch4,
                                  cb.x + 7, cb.y + 5,
                                  cb.x + 14, cb.y + 12, 0xFFFFFF, r);
                    titlebar_line(backbuffer, pitch4,
                                  cb.x + 14, cb.y + 5,
                                  cb.x + 7, cb.y + 12, 0xFFFFFF, r);
                    // Phase 10: teks judul, clamp ke area sebelum tombol close.
                    if (kwm_windows[w].title[0])
                        titlebar_text(backbuffer, pitch4, win_x + 12, win_y + 4,
                                      kwm_windows[w].title,
                                      win_x + (int32_t)cw - KWM_CLOSE_BTN_W - 8,
                                      0xFFFFFF, r);
                }
            }
        }
    }
}

// ------------------------------------------------------------
// Phase 2C §9.4 — hardware cursor
// Image 64x64 ARGB8888 di-render dari bitmap software yang sama (1=putih,
// 2=hitam, 0=transparan) supaya output visual identik. Saat aktif, kursor
// TIDAK lagi digambar ke backbuffer dan gerakannya tidak men-dirty layar —
// device meng-composite plane kursor di atas scanout.
// ------------------------------------------------------------
#define HW_CURSOR_SIZE 64
static ghal_surface_t* g_hw_cursor_surface;   // resource kursor (owner compositor)
static uint32_t*       g_cursor_img_buf;      // 64x64 ARGB scratch
static int             g_hw_cursor_active = 0;
static int             g_hw_cursor_kind = -1;

static void hw_cursor_fill(int kind) {
    const uint8_t (*bm)[12] = kind == 0 ? cursor_bitmap
                            : kind == 1 ? g_ibeam_bitmap
                            : g_hand_bitmap;
    for (int y = 0; y < HW_CURSOR_SIZE; y++) {
        uint32_t* row = g_cursor_img_buf + y * HW_CURSOR_SIZE;
        for (int x = 0; x < HW_CURSOR_SIZE; x++) {
            uint32_t c = 0x00000000;   // transparan
            if (x < CURSOR_WIDTH && y < CURSOR_HEIGHT) {
                uint8_t p = bm[y][x];
                if (p == 1) c = 0xFFFFFFFF;      // putih opaque
                else if (p == 2) c = 0xFF000000; // hitam opaque
            }
            row[x] = c;
        }
    }
}

// Fill + upload + UPDATE_CURSOR ke device. Return 0 sukses.
static int hw_cursor_set_kind(int kind) {
    if (!g_hw_cursor_active || !g_hw_cursor_surface || !g_cursor_img_buf) return -1;
    g_hw_cursor_kind = kind;
    hw_cursor_fill(kind);
    ghal_rect_t full = { 0, 0, HW_CURSOR_SIZE, HW_CURSOR_SIZE };
    ghal_surface_upload(g_hw_cursor_surface, g_cursor_img_buf, HW_CURSOR_SIZE, full);
    return ghal_cursor_update(g_hw_cursor_surface, 0, 0);
}

// Dipanggil dari compositor_ghal_init (task context — kmalloc + command
// virtqueue tidak boleh di IRQ). Gagal di titik mana pun = fallback permanen
// ke software cursor (jalur lama tetap utuh).
static void compositor_hw_cursor_init(void) {
    if (!(ghal_capabilities() & GHAL_CAP_HW_CURSOR)) return;
    g_cursor_img_buf = (uint32_t*)kmalloc(HW_CURSOR_SIZE * HW_CURSOR_SIZE * 4);
    if (!g_cursor_img_buf) return;
    g_hw_cursor_surface = ghal_surface_create(HW_CURSOR_SIZE, HW_CURSOR_SIZE, GHAL_FMT_ARGB8888);
    if (!g_hw_cursor_surface) {
        kfree(g_cursor_img_buf);
        g_cursor_img_buf = NULL;
        return;
    }
    g_hw_cursor_active = 1;
    if (hw_cursor_set_kind(g_cursor_kind) != 0) {
        // Device menolak cursor (plane tidak tersedia dsb.) — rollback.
        g_hw_cursor_active = 0;
        ghal_surface_destroy(g_hw_cursor_surface);
        g_hw_cursor_surface = NULL;
        kfree(g_cursor_img_buf);
        g_cursor_img_buf = NULL;
    }
}

// Phase 9 — ganti bentuk kursor global (0 panah / 1 I-beam / 2 tangan).
// Dipanggil dari syscall 58 (sys_kwm_set_cursor). Hardware cursor (§9.4):
// re-definisi plane device (sync, task context). Software cursor: tandai
// rect kursor dirty agar komposit berikutnya memakai bentuk baru.
void kwm_set_cursor(int kind) {
    if (kind < 0 || kind >= 3) return;
    g_cursor_kind = kind;
    if (g_hw_cursor_active) {
        (void)hw_cursor_set_kind(kind);
        return;
    }
    screen_mark_dirty(mouse_x, mouse_y, CURSOR_WIDTH, CURSOR_HEIGHT);
}

// ============================================================
// Phase 2B — present lewat Graphics HAL
//
// Compositor tetap menyusun frame di `backbuffer` (system memory) seperti
// sebelumnya — output visual identik. Yang berubah: langkah TERAKHIR
// (backbuffer → layar) tidak lagi menulis `fb_ptr` langsung, melainkan
// upload region damage ke main surface HAL lalu `ghal_present`.
//
// Batching (roadmap §8.9): seluruh dirty-rect frame di-upload, lalu SATU
// present dengan bounding rect gabungan — bukan satu present per rect.
//
// PENTING: compositor_flush berjalan di TIMER IRQ. Karena itu main surface
// dibuat SEKALI di compositor_ghal_init() (dipanggil kernel_main, konteks
// task), BUKAN lazy di dalam IRQ — surface_create memanggil kmalloc dan
// (pada backend virtio) mengirim command + busy-poll virtqueue, dua hal yang
// tidak boleh terjadi di IRQ handler.
// ============================================================
static ghal_surface_t* g_main_surface;

// Fence present terakhir (Phase 2C §9.2). 0 = belum ada / backend sync.
// Di-wait di awal flush BERIKUTNYA — sebelum upload menimpa backing yang
// mungkin masih dibaca DMA device (backpressure alami, bukan command baru
// menimpa command lama yang belum selesai).
static uint64_t g_present_fence = 0;


// Dipanggil dari kernel_main SETELAH ghal_init(), sebelum timer_callbacks_init.
void compositor_ghal_init(void) {
    if (g_main_surface != NULL) return;
    if (fb_width == 0) return;
    // Scanout surface: backend software MEMBUNGKUS framebuffer HW (zero-copy —
    // upload menulis langsung ke layar, present no-op). Backend virtio membuat
    // resource + backing seperti biasa.
    g_main_surface = ghal_surface_create_scanout(fb_width, fb_height, GHAL_FMT_XRGB8888);
    // Phase 2C §9.4 — hardware cursor bila backend mendukung (fallback aman).
    compositor_hw_cursor_init();
}

void compositor_flush() {
    if (fb_width == 0) return;
    DisplayBuffer* screen_db = gfx_screen_buffer();  // base_canvas
    DisplayBuffer* back_db   = gfx_back_buffer();
    if (!screen_db || !back_db) return;
    const int pitch4 = (int)(fb_pitch / 4);
    Rect screen = { 0, 0, fb_width, fb_height };

    uint64_t flags = spinlock_lock_irqsave(&g_dirty_lock);
    DirtyRegionList dirty = g_screen_dirty;
    dirty_region_clear(&g_screen_dirty);
    spinlock_unlock_irqrestore(&g_dirty_lock, flags);

    // Cursor moves every frame it's dragged; both the vacated and the new cell
    // must repaint, so fold them into the dirty set. Hardware cursor (§9.4):
    // gerakan TIDAK men-dirty layar (plane device yang bergeser) — cukup
    // MOVE_CURSOR; bila tak ada dirty lain, flush bisa langsung selesai.
    int32_t cx = mouse_x, cy = mouse_y;
    if (g_hw_cursor_active) {
        if (dirty.count == 0) {
            if (cx != g_last_cursor_x || cy != g_last_cursor_y) {
                ghal_cursor_move(cx, cy);
                g_last_cursor_x = cx;
                g_last_cursor_y = cy;
            }
            return;
        }
    } else {
        if (g_last_cursor_x >= 0) {
            Rect old = { g_last_cursor_x, g_last_cursor_y, CURSOR_WIDTH, CURSOR_HEIGHT };
            dirty_region_mark(&dirty, old);
        }
        Rect cur = { cx, cy, CURSOR_WIDTH, CURSOR_HEIGHT };
        dirty_region_mark(&dirty, cur);
    }

    if (dirty.count == 0) return;

    uint64_t kwm_flags = spinlock_lock_irqsave(&kwm_lock);
    for (uint32_t i = 0; i < dirty.count; i++) {
        Rect r;
        if (!rect_intersect(dirty.regions[i], screen, &r)) continue;
        blit_rect_db(back_db, screen_db, r);
        composite_windows_in_rect(r, pitch4);
    }
    spinlock_unlock_irqrestore(&kwm_lock, kwm_flags);

    if (g_hw_cursor_active) {
        // Hw cursor: kursor tidak digambar ke backbuffer — cukup MOVE_CURSOR
        // saat posisi berubah (device meng-composite plane di atas scanout).
        if (cx != g_last_cursor_x || cy != g_last_cursor_y)
            ghal_cursor_move(cx, cy);
    } else {
        const uint8_t (*cbm)[12] = g_cursor_kind == 0 ? cursor_bitmap
                         : g_cursor_kind == 1 ? g_ibeam_bitmap
                         : g_hand_bitmap;
        for (int y = 0; y < CURSOR_HEIGHT; y++) {
            for (int x = 0; x < CURSOR_WIDTH; x++) {
                // Bug 5.5: cek batas BAWAH juga — koordinat negatif membuat offset
                // bernilai negatif → write sebelum backbuffer.
                if (cy + y < 0 || cx + x < 0 ||
                    cy + y >= (int32_t)fb_height || cx + x >= (int32_t)fb_width) continue;
                uint32_t offset = ((cy + y) * pitch4) + (cx + x);
                if (cbm[y][x] == 1) backbuffer[offset] = 0xFFFFFF;
                else if (cbm[y][x] == 2) backbuffer[offset] = 0x000000;
            }
        }
    }
    g_last_cursor_x = cx;
    g_last_cursor_y = cy;

    // --- Present lewat HAL (Phase 2B) ---
    // Upload tiap region damage ke main surface. Pada backend software, surface
    // MEMBUNGKUS framebuffer HW (zero-copy: upload = tulis langsung ke layar,
    // present no-op), jadi biayanya sama dengan jalur langsung lama.
    //
    // Present: per-rect bila dirty-rect sedikit, union bila banyak.
    // Union bounding-box atas rect yang tersebar (mis. kursor teks di tengah +
    // HUD di pojok) bisa mencakup nyaris seluruh layar — jauh lebih mahal
    // daripada beberapa present kecil. Threshold di bawah menghindari itu
    // (roadmap §8.9 menyerahkan angka ini ke profiling).
    #define PRESENT_UNION_THRESHOLD 4
    if (g_main_surface != NULL) {
        // Backpressure (§9.2): pastikan present frame sebelumnya selesai
        // sebelum upload menulis backing yang sama. Steady state fence sudah
        // selesai saat flush berikutnya datang — poll single-shot, tanpa spin.
        if (g_present_fence != 0 && ghal_fence_pending(g_present_fence))
            ghal_fence_wait(g_present_fence);
        g_present_fence = 0;

        Rect rects[MAX_DIRTY_REGIONS];
        uint32_t n = 0;
        for (uint32_t i = 0; i < dirty.count && n < MAX_DIRTY_REGIONS; i++) {
            Rect r;
            if (!rect_intersect(dirty.regions[i], screen, &r)) continue;
            ghal_rect_t gr = { (uint32_t)r.x, (uint32_t)r.y, r.width, r.height };
            ghal_surface_upload(g_main_surface, backbuffer, (uint32_t)pitch4, gr);
            rects[n++] = r;
        }
        if (n == 0) return;

        if (n <= PRESENT_UNION_THRESHOLD) {
            for (uint32_t i = 0; i < n; i++) {
                ghal_rect_t gr = { (uint32_t)rects[i].x, (uint32_t)rects[i].y,
                                   rects[i].width, rects[i].height };
                ghal_present(g_main_surface, &gr);
            }
        } else {
            // Banyak rect: satu present dengan bounding box gabungan.
            Rect u = rects[0];
            for (uint32_t i = 1; i < n; i++) u = rect_union(u, rects[i]);
            ghal_rect_t gu = { (uint32_t)u.x, (uint32_t)u.y, u.width, u.height };
            ghal_present(g_main_surface, &gu);
        }
        // Backend async: ghal_present tidak blocking — simpan fence untuk
        // backpressure flush berikutnya. Backend sync: 0 (no-op).
        g_present_fence = ghal_present_fence();
    } else {
        DisplayBuffer* fb_db = gfx_fb_buffer();
        if (fb_db) {
            for (uint32_t i = 0; i < dirty.count; i++) {
                Rect r;
                if (!rect_intersect(dirty.regions[i], screen, &r)) continue;
                blit_rect_db(fb_db, back_db, r);
            }
        }
    }
}
