// libs/widget/include/primitives/button.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_PRIMITIVES_BUTTON_HPP
#define KWIDGET_PRIMITIVES_BUTTON_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/theme.hpp"

namespace ui {

// ------------------------------------------------------------
// Button — rounded rect + gradient halus, state normal/hover/pressed
// ------------------------------------------------------------
class Button : public Widget {
public:
    char* text;
    bool hover;
    bool pressed;
    Button(const char* t) : text(_ui_strdup(t)), hover(false), pressed(false) {
        // Grid 8px: padding 12px kiri/kanan, tinggi 28 (teks 16 + 6/6).
        w = _ui_strlen(text) * 8 + 24; h = 28;
        cursor_kind = UI_CURSOR_HAND;
    }
    virtual ~Button() { _ui_free(text); }
    virtual void draw(Painter& p) override {
        color_t base = pressed ? color_darken(p.theme.button_bg, SHADE_18)
                     : hover   ? p.theme.button_hover
                               : p.theme.button_bg;
        // Gradient ~10% (terang di atas; dibalik saat pressed) + sudut 6px.
        p.rrect_grad(x, y, w, h, 6,
                     pressed ? color_darken(base, SHADE_5)  : color_lighten(base, SHADE_10),
                     pressed ? color_lighten(base, SHADE_5) : color_darken(base, SHADE_10));
        p.rrect_border(x, y, w, h, 6, COLOR_BLACK, pressed ? 90 : 55);
        // Inset shadow tipis di tepi atas saat ditekan.
        if (pressed) p.blend_rect(x + 6, y + 1, w - 12, 1, COLOR_BLACK, 60);
        p.text(text, x + (w - _ui_strlen(text) * 8) / 2,
               y + (h - 16) / 2 + (pressed ? 1 : 0), p.theme.button_fg);
    }
    virtual void set_hover(bool on) override { hover = on; if (!on) pressed = false; mark_dirty(); }
    virtual void on_click(int mx, int my) override {
        pressed = true;             // render() dipanggil Window setelah ini
        mark_dirty();
        Widget::on_click(mx, my);
    }
    virtual void on_release() override { pressed = false; mark_dirty(); }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_BUTTON_HPP
