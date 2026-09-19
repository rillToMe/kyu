// libs/widget/include/layout/vbox.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_LAYOUT_VBOX_HPP
#define KWIDGET_LAYOUT_VBOX_HPP

#include "layout/layout.hpp"

namespace ui {

// ------------------------------------------------------------
// VBox — susun anak vertikal berurutan, rata kiri
// ------------------------------------------------------------
class VBox : public Layout {
public:
    int spacing;
    VBox(int s) : spacing(s) { w = 0; h = 0; }
    virtual void arrange() override {
        int cy = y;
        for (int i = 0; i < count; i++) {
            if (!children[i]->visible) continue;   // widget tersembunyi tidak makan tempat
            place(children[i], x, cy);
            cy += children[i]->h + spacing;
        }
        h = cy - y;   // ukuran diri = isi; dipakai ScrollView untuk hitung scroll
    }
};

} // namespace ui

#endif // KWIDGET_LAYOUT_VBOX_HPP
