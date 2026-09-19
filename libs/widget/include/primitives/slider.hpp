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
        int span = max - min;
        int nw = w - 8;
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
        int hx = span ? (val - min) * (w - 8) / span : 0;
        p.rect(x + hx, y, 8, h, p.theme.accent);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_SLIDER_HPP
