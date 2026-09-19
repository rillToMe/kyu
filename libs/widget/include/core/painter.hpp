// libs/widget/include/core/painter.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CORE_PAINTER_HPP
#define KWIDGET_CORE_PAINTER_HPP

#include "runtime/platform.hpp"
#include "core/theme.hpp"

namespace ui {

// ------------------------------------------------------------
// Painter — satu-satunya jembatan widget -> renderer (libgui C)
// ------------------------------------------------------------
class Painter {
public:
    gui_window_t* win;
    const Theme& theme;
    // Scissor rect widget-level (Phase 8) — set_clip/clear_clip dipakai widget.
    bool clip_on;
    int clip_x, clip_y, clip_w, clip_h;
    // Phase 5: render/dirty clip — dipasang Window::render, TIDAK disentuh widget.
    bool rclip_on;
    int rclip_x, rclip_y, rclip_w, rclip_h;
    Painter(gui_window_t* w, const Theme& t)
        : win(w), theme(t), clip_on(false), clip_x(0), clip_y(0),
          clip_w(0), clip_h(0), rclip_on(false), rclip_x(0), rclip_y(0),
          rclip_w(0), rclip_h(0) {}
    void set_render_clip(int x, int y, int w, int h) {
        rclip_on = true; rclip_x = x; rclip_y = y; rclip_w = w; rclip_h = h;
    }
    void set_clip(int x, int y, int w, int h) {
        clip_on = true; clip_x = x; clip_y = y; clip_w = w; clip_h = h;
    }
    void clear_clip() { clip_on = false; }
    // Potong rect ke scissor widget + render clip + bounds window.
    // Return false bila kosong. Semua primitif lewat sini (satu jalur clipping).
    bool clip_rect(int& x, int& y, int& w, int& h) {
        int x1 = x + w, y1 = y + h;
        if (clip_on) {
            if (x < clip_x) x = clip_x;
            if (y < clip_y) y = clip_y;
            int cx = clip_x + clip_w, cy = clip_y + clip_h;
            if (x1 > cx) x1 = cx;
            if (y1 > cy) y1 = cy;
        }
        if (rclip_on) {
            if (x < rclip_x) x = rclip_x;
            if (y < rclip_y) y = rclip_y;
            int rx = rclip_x + rclip_w, ry = rclip_y + rclip_h;
            if (x1 > rx) x1 = rx;
            if (y1 > ry) y1 = ry;
        }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x1 > (int)win->width)  x1 = (int)win->width;
        if (y1 > (int)win->height) y1 = (int)win->height;
        if (x1 <= x || y1 <= y) return false;
        w = x1 - x; h = y1 - y;
        return true;
    }
    void rect(int x, int y, int w, int h, color_t c) {
        if (!clip_rect(x, y, w, h)) return;
        // libgui memaksa alpha opaque saat menulis canvas (mask transparansi
        // window urusan compositor), jadi `c` dikirim apa adanya.
        gui_draw_rect(win, x, y, w, h, c);
    }
    void text(const char* s, int x, int y, color_t c) {
        // Tanpa clip: jalur cepat (libgui menandai damage ter-clip sendiri).
        if (!clip_on && !rclip_on) { gui_draw_text(win, s, x, y, c); return; }
        // Ter-clip: gambar per-sel 8x16; hanya sel yang beririsan dengan clip.
        // cx/cy dijejak terpisah (bukan x + i*8): setelah '\n' kolom HARUS
        // kembali ke kiri, kalau tidak baris kedua dan seterusnya melebar ke
        // kanan — inilah yang dulu merusak teks multi-baris di dialog.
        int cx = x, cy = y;
        for (int i = 0; s[i] && i < 512; i++) {
            if (s[i] == '\n') { cy += 16; cx = x; continue; }
            int rx = cx, ry = cy, rw = 8, rh = 16;
            if (clip_rect(rx, ry, rw, rh))
                gui_draw_char(win, s[i], cx, cy, c);
            cx += 8;
        }
    }
    // Blit PNG XRGB8888 (px = iw×ih) diskalakan nearest-neighbor ke rect
    // (x,y,w,h). Menulis win->canvas langsung (libgui tak punya draw-image).
    // Phase 5: clip ke window + scissor + render clip, lalu catat damage rect.
    void image(int x, int y, int w, int h, const uint32_t* px, int iw, int ih) {
        if (w <= 0 || h <= 0 || iw <= 0 || ih <= 0 || !px) return;
        int dx = x, dy = y, dw = w, dh = h;
        if (!clip_rect(dx, dy, dw, dh)) return;
        int q0 = dx - x, py0 = dy - y, q1 = q0 + dw, py1 = py0 + dh;
        int cw = (int)win->width;
        for (int py = py0; py < py1; py++) {
            int sy = py * ih / h;
            for (int q = q0; q < q1; q++) {
                int sx = q * iw / w;
                win->canvas[(y + py) * cw + x + q] = px[sy * iw + sx];
            }
        }
        gui_damage_rect(win, dx, dy, dw, dh);
    }

    // ----- Primitif "modern" (blend / gradient / rounded / shadow) -----
    // Compositor kernel memakai byte alpha canvas sebagai MASK opaque
    // (0 = tembus), bukan faktor blend → blending harus dilakukan di sini:
    // baca pixel canvas, campur, tulis kembali.
    void blend(int px, int py, color_t c, uint32_t a) {
        if (a == 0) return;
        int x = px, y = py, w = 1, h = 1;
        if (!clip_rect(x, y, w, h)) return;
        uint32_t* d = &win->canvas[y * (int)win->width + x];
        color_t src = color_with_alpha(c, (uint8_t)a);
        color_t dst = color_opaque(color_from_u32(*d, FORMAT_ARGB));
        *d = color_to_u32(color_blend_alpha(src, dst), FORMAT_ARGB);
        gui_damage_rect(win, x, y, 1, 1);
    }
    void blend_rect(int x, int y, int w, int h, color_t c, uint32_t a) {
        for (int iy = y; iy < y + h; iy++)
            for (int ix = x; ix < x + w; ix++) blend(ix, iy, c, a);
    }
    // Gradient vertikal (lerp integer per baris).
    void vgrad(int x, int y, int w, int h, color_t top, color_t bot) {
        for (int iy = 0; iy < h; iy++)
            rect(x, y + iy, w, 1, color_blend_alpha(
                color_with_alpha(bot, (uint8_t)(iy * 255 / (h > 1 ? h - 1 : 1))), top));
    }
    // Coverage 0..255 pixel (px,py) di dalam rounded-rect (aa_math.h:
    // supersample 4x4 integer — pengganti Wu yang butuh float).
    static uint32_t rr_cov(int px, int py, int x, int y, int w, int h, int r) {
        return aa_cov(px, py, x, y, w, h, r, r);
    }
    // Rounded rect + gradient vertikal, sudut anti-alias.
    void rrect_grad(int x, int y, int w, int h, int r, color_t top, color_t bot) {
        if (w <= 0 || h <= 0) return;
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        const bool flat = color_to_u32(top, FORMAT_ARGB) == color_to_u32(bot, FORMAT_ARGB);
        for (int iy = y; iy < y + h; iy++) {
            color_t c = flat ? top : color_blend_alpha(
                color_with_alpha(bot, (uint8_t)((iy - y) * 255 / (h > 1 ? h - 1 : 1))), top);
            if (iy >= y + r && iy < y + h - r) { rect(x, iy, w, 1, c); continue; }
            rect(x + r, iy, w - 2 * r, 1, c);
            for (int k = 0; k < r; k++) {
                blend(x + k, iy, c, rr_cov(x + k, iy, x, y, w, h, r));
                blend(x + w - 1 - k, iy, c, rr_cov(x + w - 1 - k, iy, x, y, w, h, r));
            }
        }
    }
    void rrect(int x, int y, int w, int h, int r, color_t c) {
        rrect_grad(x, y, w, h, r, c, c);
    }
    // Border 1px halus mengikuti sudut bulat (alpha, bukan garis keras).
    void rrect_border(int x, int y, int w, int h, int r, color_t c, uint32_t a) {
        if (w <= 0 || h <= 0) return;
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        for (int iy = y; iy < y + h; iy++) {
            if (iy >= y + r && iy < y + h - r) {
                blend(x, iy, c, a); blend(x + w - 1, iy, c, a);
                continue;
            }
            for (int k = 0; k < r; k++) {
                // Tepi arc = pixel dengan coverage partial.
                uint32_t cl = rr_cov(x + k, iy, x, y, w, h, r);
                if (cl && cl < 255) blend(x + k, iy, c, a * cl / 255);
                uint32_t cr = rr_cov(x + w - 1 - k, iy, x, y, w, h, r);
                if (cr && cr < 255) blend(x + w - 1 - k, iy, c, a * cr / 255);
            }
        }
        blend_rect(x + r, y, w - 2 * r, 1, c, a);
        blend_rect(x + r, y + h - 1, w - 2 * r, 1, c, a);
    }
    // Drop shadow: 4 ring alpha menurun, offset 3px ke bawah (blur semu).
    // Gambar SEBELUM isi widget — ring di dalam rect ditimpa widget.
    // ponytail: ring 1px, bukan gaussian blur; cukup untuk kesan kedalaman.
    void shadow(int x, int y, int w, int h) {
        static const uint32_t A[4] = { 52, 38, 24, 12 };
        for (int k = 1; k <= 4; k++) {
            int sx = x - k, sy = y - k + 3, sw = w + 2 * k, sh = h + 2 * k;
            uint32_t a = A[k - 1];
            blend_rect(sx, sy, sw, 1, COLOR_BLACK, a);
            blend_rect(sx, sy + sh - 1, sw, 1, COLOR_BLACK, a);
            blend_rect(sx, sy + 1, 1, sh - 2, COLOR_BLACK, a);
            blend_rect(sx + sw - 1, sy + 1, 1, sh - 2, COLOR_BLACK, a);
        }
    }
};

} // namespace ui

#endif // KWIDGET_CORE_PAINTER_HPP
