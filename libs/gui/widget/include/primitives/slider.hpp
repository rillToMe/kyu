// libs/widget/include/primitives/slider.hpp — slider (track + handle).
//
// Bahasa visual: track tipis 4px dengan FILL AKSEN sampai posisi nilai, lalu
// handle bulat 14px. Fill itulah yang membuat slider terbaca sebagai "nilai
// sekarang", bukan sekadar garis dengan kotak yang bisa digeser.
//
// Track memakai surface_variant (terbenam) supaya kontras dengan fill aksen
// jelas di kedua mode. Handle memakai warna permukaan + border supaya terlihat
// bisa digenggam tanpa perlu bayangan.
//
// MATEMATIKA INPUT TIDAK DIUBAH: `HANDLE_W` + `clamp_to()` memakai pemetaan
// lama (tepi kiri handle = posisi kursor) yang sudah dikunci
// tests/host/unit/libui_theme_test.cpp. Yang dimodernisasi hanya tampilannya.
#ifndef KWIDGET_PRIMITIVES_SLIDER_HPP
#define KWIDGET_PRIMITIVES_SLIDER_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/state.hpp"

namespace ui {

class Slider : public Widget {
public:
    // Lebar handle (px) — satu-satunya sumber kebenaran geometri handle:
    // dipakai draw() (menggambar handle) dan clamp_to() (ruang gerak handle).
    enum { HANDLE_W = 14, TRACK_H = 4 };
    int min, max, val;
    bool dragging;
    bool hover;
    ui_click_cb change_cb;
    void* change_data;

    Slider(int mn, int mx) : min(mn), max(mx), val(mn), dragging(false),
                             hover(false), change_cb(0), change_data(0) {
        w = 160;
        h = 20;
        if (max <= min) max = min + 1;
        cursor_kind = UI_CURSOR_HAND;
    }
    void set_value(int v) {
        if (v < min) v = min;
        if (v > max) v = max;
        if (val == v) return;
        val = v;
        mark_dirty();
    }
    void clamp_to(int mx) {
        // Widget yang lebarnya <= lebar handle tidak punya ruang gerak sama
        // sekali: nw jadi 0 → pembagian nol. Di bare-metal tanpa exception itu
        // crash/UB diam-diam, bukan sekadar nilai salah tampil. Nilai dibiarkan
        // apa adanya (fixed) — memaksa w naik di constructor akan mengubah
        // layout yang sudah di-set caller.
        int nw = w - HANDLE_W;
        if (nw < 1) return;
        int span = max - min;
        set_value(min + (mx - x) * span / nw);   // mx - x = posisi dalam widget
    }
    virtual bool focusable() override { return enabled; }
    virtual void set_hover(bool onv) override {
        if (hover == onv) return;
        hover = onv;
        mark_dirty();
    }
    virtual bool on_drag(int mx, int my) override {
        (void)my;
        if (!enabled || !dragging) return false;
        int old = val;
        clamp_to(mx);
        if (val != old && change_cb) change_cb(change_data);
        return true;
    }
    virtual void on_release() override {
        if (!dragging) return;
        dragging = false;
        mark_dirty();
    }
    virtual void on_click(int mx, int my) override {
        (void)my;
        if (!enabled) return;
        dragging = true;
        int old = val;
        clamp_to(mx);
        mark_dirty();
        if (val != old && change_cb) change_cb(change_data);
    }
    // Keyboard: panah ±1, PgUp/PgDn ±sepuluh rentang, Home/End ujung.
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        (void)ascii; (void)mods;
        if (!enabled) return;
        uint32_t sc = scancode & 0xFF;
        int span = max - min;
        int step = span / 10;
        if (step < 1) step = 1;
        int old = val, nv = val;
        switch (sc) {
        case 0x4B: nv = val - 1; break;        // Left
        case 0x4D: nv = val + 1; break;        // Right
        case 0x49: nv = val - step; break;     // PgUp
        case 0x51: nv = val + step; break;     // PgDn
        case 0x47: nv = min; break;            // Home
        case 0x4F: nv = max; break;            // End
        default: return;
        }
        set_value(nv);
        if (val != old && change_cb) change_cb(change_data);
    }
    virtual void draw(Painter& p) override {
        StateInputs st;
        st.hover = hover;
        st.focused = has_focus;
        st.enabled = enabled;
        const bool active = enabled && (hover || dragging);

        const int cy = y + h / 2;
        const int ty = cy - TRACK_H / 2;
        int nw = w - HANDLE_W;
        if (nw < 0) nw = 0;
        int span = max - min;
        int hx = x + (span > 0 ? (val - min) * nw / span : 0);
        // Fill berakhir di TENGAH handle supaya tidak ada celah di ujung kiri.
        int fill_w = (hx - x) + HANDLE_W / 2;
        if (fill_w < 0) fill_w = 0;
        if (fill_w > w) fill_w = w;

        // Track (terbenam) + fill (nilai sekarang).
        p.surface(x, ty, w, TRACK_H, p.theme.surface_variant, radius::PILL);
        if (fill_w > 0) {
            color_t fc = !enabled ? p.theme.text_disabled
                       : active  ? p.theme.accent_hover
                                 : p.theme.accent;
            p.surface(x, ty, fill_w, TRACK_H, fc, radius::PILL);
        }
        // Handle: bulat, permukaan + border. Fokus = ring fokus (bukan
        // perubahan warna handle) supaya "keyboard ada di sini" terbaca jelas
        // tanpa mengubah makna warna handle (yang berarti "aktif/di-hover").
        color_t hf = enabled ? p.theme.surface : p.theme.surface_variant;
        color_t hb = !enabled  ? p.theme.border_subtle
                   : has_focus ? p.theme.focus
                   : active    ? p.theme.accent
                               : p.theme.border;
        int hy = cy - HANDLE_W / 2;
        p.surface(hx, hy, HANDLE_W, HANDLE_W, hf, radius::PILL);
        p.rrect_border(hx, hy, HANDLE_W, HANDLE_W, radius::PILL, hb, 255);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_SLIDER_HPP
