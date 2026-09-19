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
    Label(const char* t) : text(_ui_strdup(t)) { w = _ui_strlen(text) * 8; h = 16; }
    virtual ~Label() { _ui_free(text); }
    void set_text(const char* t) {
        char* n = _ui_strdup(t);
        if (!n) return;
        mark_dirty();               // bounds lama (w bisa menyusut)
        _ui_free(text);
        text = n;
        w = _ui_strlen(text) * 8;
        mark_dirty();               // bounds baru
    }
    virtual void draw(Painter& p) override { p.text(text, x, y, p.theme.fg); }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_LABEL_HPP
