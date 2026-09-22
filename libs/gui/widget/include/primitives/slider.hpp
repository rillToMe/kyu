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
    ui_click_cb change_cb;
    void* change_data;

    Slider(int mn, int mx) : min(mn), max(mx), val(mn), dragging(false),
                             change_cb(0), change_data(0) {
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
    virtual bool on_drag(int mx, int my) override {
        (void)my;
        if (!dragging) return false;
        int old = val;
        clamp_to(mx);
        if (val != old && change_cb) change_cb(change_data);
        return true;
    }
    virtual void on_release() override { dragging = false; }
    virtual void on_click(int mx, int my) override {
        (void)my;
        dragging = true;
        int old = val;
        clamp_to(mx);
        if (val != old && change_cb) change_cb(change_data);
    }
    virtual void draw(Painter& p) override {
        p.rect(x, y + h / 2 - 2, w, 4, p.theme.button_bg);
        int span = max - min;
        int hx = span ? (val - min) * (w - HANDLE_W) / span : 0;
        p.rect(x + hx, y, HANDLE_W, h, p.theme.accent);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_SLIDER_HPP
