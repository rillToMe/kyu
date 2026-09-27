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
    bool hover;
    ui_click_cb toggle_cb;
    void* toggle_data;

    CheckBox(const char* t) : label(_ui_strdup(t)), checked(false), hover(false),
                              toggle_cb(0), toggle_data(0) {
        w = _ui_strlen(label) * 8 + 20; h = 20;
    }
    virtual ~CheckBox() { _ui_free(label); }
    void set_checked(bool c) { checked = c; mark_dirty(); }
    virtual bool focusable() override { return enabled; }
    virtual void set_hover(bool on) override { hover = on; mark_dirty(); }
    virtual void on_click(int mx, int my) override {
        (void)mx; (void)my;
        if (!enabled) return;
        checked = !checked;
        mark_dirty();
        if (toggle_cb) toggle_cb(toggle_data);
    }
    // Keyboard: Enter/Spasi toggle saat fokus.
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        (void)mods;
        if (!enabled) return;
        uint32_t sc = scancode & 0xFF;
        if (ascii == '\n' || ascii == '\r' || ascii == ' ' || sc == 0x1C)
            on_click(0, 0);
    }
    virtual void draw(Painter& p) override {
        // Unchecked: permukaan + border (menegas saat hover/fokus).
        // Checked: isi aksen + centang kontras (terbaca di kedua mode).
        color_t box = !enabled ? p.theme.surface
                    : checked  ? p.theme.accent
                               : p.theme.surface;
        color_t bd = !enabled      ? p.theme.border_subtle
                   : has_focus     ? p.theme.focus
                   : hover         ? p.theme.text
                                   : p.theme.border;
        p.rect(x, y, 12, 12, box);
        p.rect(x, y, 12, 1, bd);
        p.rect(x, y + 11, 12, 1, bd);
        p.rect(x, y, 1, 12, bd);
        p.rect(x + 11, y, 1, 12, bd);
        if (checked) {                            // centang diagonal kontras
            color_t mk = enabled ? p.theme.accent_contrast : p.theme.text_disabled;
            for (int i = 0; i < 4; i++) p.rect(x + 2 + i, y + 6 + i, 1, 1, mk);
            for (int i = 0; i < 6; i++) p.rect(x + 6 + i, y + 9 - i, 1, 1, mk);
        }
        p.text(label, x + 20, y + 2, enabled ? p.theme.text : p.theme.text_disabled);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_CHECKBOX_HPP
