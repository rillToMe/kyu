// libs/widget/include/layout/hbox.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_LAYOUT_HBOX_HPP
#define KWIDGET_LAYOUT_HBOX_HPP

#include "layout/layout.hpp"

namespace ui {

// ------------------------------------------------------------
// HBox — susun anak horizontal (grid tombol kalkulator, Phase 10)
// ------------------------------------------------------------
class HBox : public Layout {
public:
    int spacing;
    HBox(int s) : spacing(s) { w = 0; h = 0; }
    virtual void arrange() override {
        int cx = x;
        int mh = 0;
        for (int i = 0; i < count; i++) {
            if (!children[i]->visible) continue;   // widget tersembunyi tidak makan tempat
            place(children[i], cx, y);
            cx += children[i]->w + spacing;
            if (children[i]->h > mh) mh = children[i]->h;
        }
        w = cx - x;
        h = mh;   // VBox luar memakai h ini untuk stack
    }
};

} // namespace ui

#endif // KWIDGET_LAYOUT_HBOX_HPP
