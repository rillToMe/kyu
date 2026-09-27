// libs/widget/include/primitives/slider.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_PRIMITIVES_SLIDER_HPP
#define KWIDGET_PRIMITIVES_SLIDER_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// Slider — track + handle yang bisa diseret (Phase 7)
// ------------------------------------------------------------
class Slider : public Widget {
public:
    // Lebar handle (px) — satu-satunya sumber kebenaran geometri handle:
    // dipakai draw() (menggambar handle) dan clamp_to() (ruang gerak handle).
    enum { HANDLE_W = 8 };
    int min, max, val;
    bool dragging;
    bool hover;
    ui_click_cb change_cb;
    void* change_data;

    Slider(int mn, int mx) : min(mn), max(mx), val(mn), dragging(false),
                             hover(false), change_cb(0), change_data(0) {
        w = 160; h = 20;
        if (max <= min) max = min + 1;
    }
    void set_value(int v) {
        if (v < min) v = min;
        if (v > max) v = max;
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
    virtual void set_hover(bool on) override { hover = on; mark_dirty(); }
    virtual bool on_drag(int mx, int my) override {
        (void)my;
        if (!enabled || !dragging) return false;
        int old = val;
        clamp_to(mx);
        if (val != old && change_cb) change_cb(change_data);
        return true;
    }
    virtual void on_release() override { dragging = false; }
    virtual void on_click(int mx, int my) override {
        (void)my;
        if (!enabled) return;
        dragging = true;
        int old = val;
        clamp_to(mx);
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
        // Track 4px + outline 1px (definisi di Light Mode) + thumb aksen.
        int cy = y + h / 2;
        color_t edge = !enabled      ? p.theme.border_subtle
                     : has_focus     ? p.theme.focus
                                     : p.theme.border_subtle;
        p.rect(x, cy - 3, w, 6, edge);
        p.rect(x + 1, cy - 2, w - 2, 4, p.theme.surface_elevated);
        int span = max - min;
        int hx = span ? (val - min) * (w - HANDLE_W) / span : 0;
        color_t th = !enabled            ? p.theme.text_disabled
                   : (hover || dragging) ? p.theme.accent_hover
                                         : p.theme.accent;
        p.rect(x + hx, y, HANDLE_W, h, th);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_SLIDER_HPP
