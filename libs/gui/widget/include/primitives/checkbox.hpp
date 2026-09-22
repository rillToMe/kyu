// libs/widget/include/primitives/checkbox.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_PRIMITIVES_CHECKBOX_HPP
#define KWIDGET_PRIMITIVES_CHECKBOX_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// CheckBox — kotak centang + label; klik toggle (Phase 7)
// ------------------------------------------------------------
class CheckBox : public Widget {
public:
    char* label;
    bool checked;
    ui_click_cb toggle_cb;
    void* toggle_data;

    CheckBox(const char* t) : label(_ui_strdup(t)), checked(false),
                              toggle_cb(0), toggle_data(0) {
        w = _ui_strlen(label) * 8 + 20; h = 20;
    }
    virtual ~CheckBox() { _ui_free(label); }
    void set_checked(bool c) { checked = c; mark_dirty(); }
    virtual void on_click(int mx, int my) override {
        (void)mx; (void)my;
        checked = !checked;
        mark_dirty();
        if (toggle_cb) toggle_cb(toggle_data);
    }
    virtual void draw(Painter& p) override {
        p.rect(x, y, 12, 12, p.theme.button_bg);
        p.rect(x, y, 12, 1, p.theme.fg);
        p.rect(x, y + 11, 12, 1, p.theme.fg);
        p.rect(x, y, 1, 12, p.theme.fg);
        p.rect(x + 11, y, 1, 12, p.theme.fg);
        if (checked) {                            // centang diagonal accent
            for (int i = 0; i < 4; i++) p.rect(x + 2 + i, y + 6 + i, 1, 1, p.theme.accent);
            for (int i = 0; i < 6; i++) p.rect(x + 6 + i, y + 9 - i, 1, 1, p.theme.accent);
        }
        p.text(label, x + 20, y + 2, p.theme.fg);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_CHECKBOX_HPP
