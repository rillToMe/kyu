// libs/widget/include/primitives/label.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_PRIMITIVES_LABEL_HPP
#define KWIDGET_PRIMITIVES_LABEL_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// Label — teks statis, lebar otomatis = panjang * 8px
// ------------------------------------------------------------
class Label : public Widget {
public:
    char* text;
    Label(const char* t) : text(_ui_strdup(t)) { w = text_measure(text); h = text_line_height(); }
    virtual ~Label() { _ui_free(text); }
    void set_text(const char* t) {
        char* n = _ui_strdup(t);
        if (!n) return;
        mark_dirty();               // bounds lama (w bisa menyusut)
        _ui_free(text);
        text = n;
        w = text_measure(text);
        mark_dirty();               // bounds baru
    }
    virtual void draw(Painter& p) override { p.text(text, x, y, p.theme.text); }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_LABEL_HPP
