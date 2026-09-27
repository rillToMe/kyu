// libs/widget/include/primitives/progressbar.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_PRIMITIVES_PROGRESSBAR_HPP
#define KWIDGET_PRIMITIVES_PROGRESSBAR_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// ProgressBar — fill horizontal read-only, 0..100 (Phase 7)
// ------------------------------------------------------------
class ProgressBar : public Widget {
public:
    int val;
    ProgressBar(int width) : val(0) { w = width; h = 16; }
    void set_value(int v) {
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        val = v;
        mark_dirty();
    }
    virtual void draw(Painter& p) override {
        // Outline 1px agar track terlihat di Light Mode (permukaan putih di
        // atas background terang); isi aksen. Tanpa teks/animasi (Phase B).
        p.rect(x, y, w, h, p.theme.border_subtle);
        if (w <= 2 || h <= 2) return;
        p.rect(x + 1, y + 1, w - 2, h - 2, p.theme.surface_elevated);
        int fw = val * (w - 2) / 100;
        if (fw > 0) p.rect(x + 1, y + 1, fw, h - 2, p.theme.accent);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_PROGRESSBAR_HPP
