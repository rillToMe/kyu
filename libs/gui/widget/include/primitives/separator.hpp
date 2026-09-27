// libs/widget/include/primitives/separator.hpp — Separator (Phase D).
#ifndef KWIDGET_PRIMITIVES_SEPARATOR_HPP
#define KWIDGET_PRIMITIVES_SEPARATOR_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// Separator — garis visual non-interaktif (horizontal/vertikal).
// Tebal tetap 1px (skala piksel toolkit); panjang via ui_widget_set_size
// (konvensi existing). Transparan terhadap mouse (pick = 0), tak focusable,
// tak masuk traversal. Warna: border_subtle (tanpa warna khusus).
// ------------------------------------------------------------
class Separator : public Widget {
public:
    bool vertical;
    Separator(bool v) : vertical(v) { w = v ? 1 : 0; h = v ? 0 : 1; }
    virtual Widget* pick(int mx, int my) override {
        (void)mx; (void)my;
        return 0;   // klik menembus ke widget di bawahnya
    }
    virtual void draw(Painter& p) override {
        if (vertical) {
            if (h <= 0) return;
            p.rect(x + w / 2, y, 1, h, p.theme.border_subtle);
        } else {
            if (w <= 0) return;
            p.rect(x, y + h / 2, w, 1, p.theme.border_subtle);
        }
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_SEPARATOR_HPP
