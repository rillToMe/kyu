#include <stdint.h>
#include <string.h>   // Phase 14: memcpy — fast-path window opaque
#include "gfx.h"
#include "kwm_internal.h"
#include "display.h"
#include "spinlock.h"
#include "aa_math.h"
#include "heap.h"     // kmalloc/kfree — buffer image kursor HW (§9.4)
#include "ghal.h"     // Phase 2B: present lewat Graphics HAL
#include "panic.h"    // lockdown: hentikan present saat BSOD aktif

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

// ------------------------------------------------------------
// Phase 16/17/18 — base-blit elimination around opaque windows
//
// Why the base copy can be skipped for a sub-rect: if a fully-opaque window's
// CONTENT rect contains that sub-rect, composition clips the window's content
// to it and — because the window passed the Phase 14 kernel opacity validation
// — writes EVERY pixel of it. Windows below are overwritten first; windows
// above only draw on top (their transparent pixels leave the opaque layer
// showing). So those pixels never depend on base_canvas, and copying base ->
// backbuffer for them is pure waste.
//
// Phase 16: full rect inside one opaque window. Phase 17: split around the
// LARGEST opaque intersection. Phase 18: consume MORE THAN ONE opaque window
// per region, greedily by how much of the still-uncovered remainder each one
// covers, subtracting each cover in turn.
//
// Composition is UNCHANGED and still runs exactly ONCE over the original rect
// (compositor_flush calls composite_windows_in_rect(r) once); only the base
// copy is decomposed. The remainder list is bounded (KWM_MAX_SPLIT_RECTS) and
// the number of windows consumed is bounded (KWM_MAX_OPAQUE_SPLITS); when a
// bound is hit the unresolved remainder is simply base-blitted, which is always
// safe (over-invalidation). When uncertain we keep the base blit: a false
// negative costs performance, a false positive would corrupt the frame.
// ------------------------------------------------------------

// Don't split for covers smaller than this (pixels); the extra blit calls cost
// more than the bytes saved. A cover that removes a whole remaining rect is
// always used, however small. Tunable; 32x32.
#define KWM_SPLIT_MIN_PIXELS   1024
// Consume at most this many opaque windows per dirty region.
#define KWM_MAX_OPAQUE_SPLITS  4
// Bounded remainder list (stack, no allocation). Fragmentation cannot grow past
// this; the split stops and the rest is base-blitted.
#define KWM_MAX_SPLIT_RECTS    12

// 1 if `inner` lies fully inside `outer` (64-bit safe; Rect may be negative).
static int rect_covers(Rect outer, Rect inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           (int64_t)inner.x + inner.width  <= (int64_t)outer.x + outer.width &&
           (int64_t)inner.y + inner.height <= (int64_t)outer.y + outer.height;
}

// Append a non-empty strip; on overflow set the flag (caller aborts the split).
static void split_push(Rect* out, int* m, int* overflow, Rect p) {
    if (p.width == 0 || p.height == 0) return;
    if (*m >= KWM_MAX_SPLIT_RECTS) { *overflow = 1; return; }
    out[(*m)++] = p;
}

// Content rect of window w, mirroring composite_windows_in_rect geometry.
static Rect window_content_rect(int w) {
    const uint32_t wflags = kwm_windows[w].flags;
    const uint32_t tb_h = (wflags & KWM_WIN_DESKTOP) ? 0 : KWM_TITLEBAR_H;
    Rect c = { kwm_windows[w].x, kwm_windows[w].y + (int32_t)tb_h,
               kwm_windows[w].width, kwm_windows[w].height };
    return c;
}

// Base-blit only the parts of `r` NOT covered by validated opaque window
// content. Caller must hold kwm_lock and call composite_windows_in_rect(r).
static void base_blit_for_region(DisplayBuffer* back_db, DisplayBuffer* screen_db,
                                 Rect r) {
    Rect remain[KWM_MAX_SPLIT_RECTS];
    Rect out[KWM_MAX_SPLIT_RECTS];
    int  n = 1;
    remain[0] = r;

    for (int step = 0; step < KWM_MAX_OPAQUE_SPLITS && n > 0; step++) {
        // Greedy: pick the opaque window whose content covers the most of what
        // is still uncovered. A window already subtracted scores 0 and is not
        // re-picked. Bounded scan: MAX_WINDOWS * n, both small.
        Rect     best = r;
        uint64_t best_score = 0;
        int      best_full = 0, have = 0;
        for (int w = 0; w < MAX_WINDOWS; w++) {
            if (!(kwm_windows[w].active && kwm_windows[w].canvas)) continue;
            if (!kwm_windows[w].fully_opaque) continue;
            if (kwm_windows[w].z_index > next_z_index) continue;  // not composited
            Rect crect = window_content_rect(w);
            uint64_t score = 0;
            int full = 0;
            for (int i = 0; i < n; i++) {
                Rect t;
                if (rect_intersect(crect, remain[i], &t)) {
                    score += (uint64_t)t.width * t.height;
                    if (rect_covers(crect, remain[i])) full = 1;
                }
            }
            if (score > best_score) { best = crect; best_score = score;
                                      best_full = full; have = 1; }
        }
        if (!have) break;                                   // nothing useful left
        // Small-cover guard: skip tiny gains unless a whole rect is removed.
        if (best_score < KWM_SPLIT_MIN_PIXELS && !best_full) break;

        // Subtract `best` from every remaining rect into out[] (up to 4 strips
        // each). If the bounded list would overflow, abandon this step and keep
        // `remain` untouched — the unresolved area is then base-blitted.
        int m = 0, overflow = 0;
        for (int i = 0; i < n; i++) {
            Rect R = remain[i], C;
            if (!rect_intersect(R, best, &C)) { split_push(out, &m, &overflow, R); continue; }
            const int64_t rx = R.x, ry = R.y, rx1 = rx + R.width,  ry1 = ry + R.height;
            const int64_t cx = C.x, cy = C.y, cx1 = cx + C.width,  cy1 = cy + C.height;
            if (cy > ry)   { Rect t = { R.x, R.y, R.width, (uint32_t)(cy - ry) };
                             split_push(out, &m, &overflow, t); }
            if (cy1 < ry1) { Rect b = { R.x, (int32_t)cy1, R.width, (uint32_t)(ry1 - cy1) };
                             split_push(out, &m, &overflow, b); }
            if (cx > rx)   { Rect l = { R.x, (int32_t)cy, (uint32_t)(cx - rx), C.height };
                             split_push(out, &m, &overflow, l); }
            if (cx1 < rx1) { Rect rt = { (int32_t)cx1, (int32_t)cy,
                                         (uint32_t)(rx1 - cx1), C.height };
                             split_push(out, &m, &overflow, rt); }
        }
        if (overflow) break;
        for (int i = 0; i < m; i++) remain[i] = out[i];
        n = m;
    }

    for (int i = 0; i < n; i++)
        blit_rect_db(back_db, screen_db, remain[i]);
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

// Garis tepi 1px tepat di luar frame (kiri/kanan/bawah). Pemisah tegas antara
// window (terutama konten terang) dan desktop. Digambar SEBELUM konten window,
// jadi hanya menyentuh piksel di luar badan konten — tepi bawah yang tadinya
// kosong (shadow bawah offset ~3px lebih ke bawah) kini jelas. Sisi atas sudah
// dibedakan oleh titlebar terang; sudut bawah tetap mengikuti geometri konten.
static void frame_edge(uint32_t* fb, int pitch4, Rect frame, Rect clip) {
    Rect l = { frame.x - 1, frame.y, 1, frame.height };
    Rect r = { frame.x + (int)frame.width, frame.y, 1, frame.height };
    Rect b = { frame.x - 1, frame.y + (int)frame.height, frame.width + 2, 1 };
    blend_rect_clip(fb, pitch4, l, KWM_EDGE_COLOR, KWM_EDGE_ALPHA, clip);
    blend_rect_clip(fb, pitch4, r, KWM_EDGE_COLOR, KWM_EDGE_ALPHA, clip);
    blend_rect_clip(fb, pitch4, b, KWM_EDGE_COLOR, KWM_EDGE_ALPHA, clip);
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
//
// ------------------------------------------------------------
// Phase 20 — opaque occlusion culling
//
// A window W only needs to write the pixels that are NOT already destined to be
// overwritten by a VALIDATED fully-opaque window drawn in FRONT of W. Such a
// window writes every pixel of its content rect when composited (Phase 14), so
// anything W draws underneath it is dead work. We therefore render W's content
// only over `clip MINUS union(front opaque content rects)`.
//
// Only `fully_opaque` windows may occlude — never a normal window, never a
// titlebar/shadow/decoration (only the kernel-validated content canvas). The
// subtraction is rectangle-based and bounded; on overflow we fall back to the
// full clip (more work, still correct).
// ------------------------------------------------------------
#define KWM_MAX_OCCLUSION_RECTS 8   // bounded visible-area list (stack, no alloc)

// Append up to four disjoint pieces of R minus C to out[] (capacity cap).
// Returns 0 if the result would exceed cap (caller must fall back safely).
// Pieces are emitted only when non-empty, so they exactly partition R minus C.
static int rect_subtract(Rect R, Rect C, Rect* out, int* m, int cap) {
    Rect I;
    if (!rect_intersect(R, C, &I)) {
        if (*m >= cap) return 0;
        out[(*m)++] = R;
        return 1;
    }
    const int64_t rx = R.x, ry = R.y, rx1 = rx + R.width,  ry1 = ry + R.height;
    const int64_t cx = I.x, cy = I.y, cx1 = cx + I.width,  cy1 = cy + I.height;
    if (cy > ry)   { if (*m >= cap) return 0; out[(*m)++] = (Rect){ R.x, R.y, R.width, (uint32_t)(cy - ry) }; }
    if (cy1 < ry1) { if (*m >= cap) return 0; out[(*m)++] = (Rect){ R.x, (int32_t)cy1, R.width, (uint32_t)(ry1 - cy1) }; }
    if (cx > rx)   { if (*m >= cap) return 0; out[(*m)++] = (Rect){ R.x, (int32_t)cy, (uint32_t)(cx - rx), I.height }; }
    if (cx1 < rx1) { if (*m >= cap) return 0; out[(*m)++] = (Rect){ (int32_t)cx1, (int32_t)cy, (uint32_t)(rx1 - cx1), I.height }; }
    return 1;
}

// Copy a window's content canvas into the backbuffer over `c` (a sub-rect of its
// content area, already clipped to the dirty rect). `opaque` = Phase 14
// validated fully-opaque canvas -> bulk row copy; else scalar alpha test.
static void mix_content(const DisplayBuffer* canvas, int32_t win_x, int32_t cty,
                        int pitch4, Rect c, int opaque) {
    for (uint32_t yy = 0; yy < c.height; yy++) {
        int32_t sy = c.y + (int32_t)yy - cty;
        const uint32_t* src = canvas->pixels +
            (uint32_t)sy * canvas->stride + (uint32_t)(c.x - win_x);
        uint32_t* dst = backbuffer +
            ((uint32_t)c.y + yy) * (uint32_t)pitch4 + (uint32_t)c.x;
        if (opaque) {
            memcpy(dst, src, (size_t)c.width * sizeof(uint32_t));
        } else {
            for (uint32_t xx = 0; xx < c.width; xx++) {
                uint32_t pixel = src[xx];
                // Alpha byte acts as a per-pixel mask: 0 = transparent.
                if (pixel >> 24) dst[xx] = pixel & 0xFFFFFF;
            }
        }
    }
}

static void composite_windows_in_rect(Rect r, int pitch4) {
    // Phase 20: opaque occluders whose content intersects r, computed once.
    // Only validated fully-opaque, composited windows qualify.
    int      occ_slot[MAX_WINDOWS];
    uint32_t occ_z[MAX_WINDOWS];
    Rect     occ_rect[MAX_WINDOWS];
    int      nocc = 0;
    for (int v = 0; v < MAX_WINDOWS; v++) {
        if (!(kwm_windows[v].active && kwm_windows[v].canvas)) continue;
        if (!kwm_windows[v].fully_opaque) continue;
        if (kwm_windows[v].z_index > next_z_index) continue;   // not composited
        Rect cr = window_content_rect(v);
        Rect t;
        if (!rect_intersect(cr, r, &t)) continue;
        occ_slot[nocc] = v;
        occ_z[nocc]    = kwm_windows[v].z_index;
        occ_rect[nocc] = cr;
        nocc++;
    }

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
                frame_edge(backbuffer, pitch4, frame, r);
            }

            // --- Konten (canvas = konten murni) ---
            Rect crect = { win_x, cty, cw, ch };
            Rect clip;
            if (rect_intersect(crect, r, &clip)) {
                const int win_opaque = kwm_windows[w].fully_opaque;
#ifdef FORCE_SCALAR_COMPOSITOR
                // TEMPORARY (Phase 14): -DFORCE_SCALAR_COMPOSITOR rebuilds the
                // pre-Phase-14 scalar composition for a pixel-identical diff.
                const int win_opaque_final = 0; (void)win_opaque;
#else
                const int win_opaque_final = win_opaque;
#endif
                // Phase 20: drop the pixels already covered by a fully-opaque
                // window drawn IN FRONT of this one (higher z, or same z and a
                // later slot). vis = clip minus that union; nvis == 0 means the
                // whole content is occluded. On bounded overflow, fall back to
                // the full clip (safe, just no culling this window).
                if (nocc == 0) {
                    mix_content(canvas, win_x, cty, pitch4, clip, win_opaque_final);
                } else {
                    Rect vis[KWM_MAX_OCCLUSION_RECTS];
                    vis[0] = clip;
                    int nvis = 1, overflow = 0;
                    for (int i = 0; i < nocc && !overflow; i++) {
                        const int front = (occ_z[i] > z) ||
                                          (occ_z[i] == z && occ_slot[i] > w);
                        if (!front) continue;
                        Rect next[KWM_MAX_OCCLUSION_RECTS];
                        int m = 0;
                        for (int k = 0; k < nvis; k++)
                            if (!rect_subtract(vis[k], occ_rect[i], next, &m,
                                               KWM_MAX_OCCLUSION_RECTS)) {
                                overflow = 1; break;
                            }
                        if (overflow) break;
                        for (int k = 0; k < m; k++) vis[k] = next[k];
                        nvis = m;
                        if (nvis == 0) break;   // fully occluded
                    }
                    if (overflow) {
                        mix_content(canvas, win_x, cty, pitch4, clip, win_opaque_final);
                    } else {
                        for (int k = 0; k < nvis; k++)
                            mix_content(canvas, win_x, cty, pitch4, vis[k], win_opaque_final);
                    }
                }
            }

            // --- Titlebar milik WM (kecuali desktop: frameless) ---
            if (!(wflags & KWM_WIN_DESKTOP)) {
                Rect tb = { win_x, win_y, cw, KWM_TITLEBAR_H };
                if (rect_intersect(tb, r, &clip)) {
                    // Chrome netral: isian rata (tanpa gradient biru), sudut
                    // atas membulat. Fokus hanya sedikit lebih terang.
                    uint32_t base = (w == focused_win_id) ? KWM_TITLEBAR_COLOR
                                                          : KWM_TITLEBAR_INACT;
                    round_grad_fill(backbuffer, pitch4, tb, base, base,
                                    KWM_CORNER_R, 0, r);
                    // Tombol close: hit area penuh-tinggi di tepi kanan.
                    // Normal: transparan + ikon abu. Hover: latar merah + ikon putih.
                    Rect cb = { win_x + (int32_t)cw - KWM_CLOSE_BTN_W, win_y,
                                KWM_CLOSE_BTN_W, KWM_TITLEBAR_H };
                    int hovered = (hovered_close_win == w + 1);
                    uint32_t ico = KWM_CTL_FG;
                    if (hovered) {
                        fill_rect_clip(backbuffer, pitch4, cb, KWM_CLOSE_HOVER_BG, r);
                        ico = KWM_CLOSE_HOVER_FG;
                    }
                    // Glyph "X": dua garis tipis 11x11, terpusat di hit area.
                    int ccx = cb.x + (int)KWM_CLOSE_BTN_W / 2;
                    int ccy = win_y + (int)KWM_TITLEBAR_H / 2;
                    titlebar_line(backbuffer, pitch4, ccx - 5, ccy - 5,
                                  ccx + 5, ccy + 5, ico, r);
                    titlebar_line(backbuffer, pitch4, ccx + 5, ccy - 5,
                                  ccx - 5, ccy + 5, ico, r);
                    // Judul: padding kiri 12px, vertikal tengah, teks gelap.
                    if (kwm_windows[w].title[0])
                        titlebar_text(backbuffer, pitch4, win_x + 12,
                                      win_y + ((int)KWM_TITLEBAR_H - 16) / 2,
                                      kwm_windows[w].title,
                                      win_x + (int32_t)cw - KWM_CLOSE_BTN_W - 8,
                                      KWM_TITLE_FG, r);
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
static int             g_panic_cursor_hidden = 0;   // kursor hw sudah di-off saat panic

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
    const display_mode_t* m = display_get_mode();
    if (!m) return;
    // Scanout surface: backend software MEMBUNGKUS framebuffer HW (zero-copy —
    // upload menulis langsung ke layar, present no-op). Backend virtio membuat
    // resource + backing seperti biasa.
    g_main_surface = ghal_surface_create_scanout(m->width, m->height, GHAL_FMT_XRGB8888);
    // Phase 2C §9.4 — hardware cursor bila backend mendukung (fallback aman).
    compositor_hw_cursor_init();
}

// Sembunyikan plane kursor hardware SEKALI saat panic lockdown.
// Dipanggil dari cb_flush (timer IRQ) — bukan dari compositor_flush, karena
// compositor_flush sudah tidak pernah dipanggil lagi setelah panic. Tanpa ini
// kursor hw tetap mengapung beku di atas BSOD (framebuffer software tidak bisa
// menghapusnya; plane device yang harus dipindah).
void compositor_panic_cursor_off(void) {
    if (g_panic_cursor_hidden) return;
    g_panic_cursor_hidden = 1;
    if (g_hw_cursor_active) ghal_cursor_move(-64, -64);
}

void compositor_flush() {
    // PANIC LOCKDOWN: BSOD digambar langsung ke framebuffer. Jangan
    // recomposite / gerakkan kursor — kalau tidak, desktop menimpa layar
    // panic tiap kali mouse bergerak.
    if (panic_is_locked()) {
        compositor_panic_cursor_off();
        return;
    }

    const display_mode_t* mode = display_get_mode();
    if (!mode) return;
    DisplayBuffer* screen_db = gfx_screen_buffer();  // base_canvas
    DisplayBuffer* back_db   = gfx_back_buffer();
    if (!screen_db || !back_db) return;
    const int pitch4 = (int)(mode->pitch_bytes / 4);
    Rect screen = { 0, 0, mode->width, mode->height };

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
        // Phase 16/17: base-blit only the parts not covered by an opaque
        // window; then compose the whole rect exactly as before.
        base_blit_for_region(back_db, screen_db, r);
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
                    cy + y >= (int32_t)mode->height || cx + x >= (int32_t)mode->width) continue;
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
