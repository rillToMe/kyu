// libs/widget/include/primitives/button.hpp — tombol state-based (Phase B).
#ifndef KWIDGET_PRIMITIVES_BUTTON_HPP
#define KWIDGET_PRIMITIVES_BUTTON_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/theme.hpp"

namespace ui {

// ------------------------------------------------------------
// Button — flat, varian SECONDARY (default) / PRIMARY / DANGER.
// State: normal / hover / pressed / focused / disabled.
// Radius 6px satu-satunya di toolkit (alasan: tombol = call-to-action;
// kontainer tetap persegi = rasa native desktop). Tanpa gradien (Phase B).
// ------------------------------------------------------------
class Button : public Widget {
public:
    char* text;
    bool hover;
    bool pressed;
    int variant;
    Button(const char* t) : text(_ui_strdup(t)), hover(false), pressed(false),
                            variant(UI_BUTTON_SECONDARY) {
        // Grid 8px: padding 12px kiri/kanan, tinggi 28 (teks 16 + 6/6).
        w = _ui_strlen(text) * 8 + 24; h = 28;
        cursor_kind = UI_CURSOR_HAND;
    }
    virtual ~Button() { _ui_free(text); }
    void set_variant(int v) {
        variant = (v == UI_BUTTON_PRIMARY || v == UI_BUTTON_DANGER)
                      ? v : UI_BUTTON_SECONDARY;
        mark_dirty();
    }
    virtual bool focusable() override { return enabled; }
    virtual void draw(Painter& p) override {
        color_t fill, txt, bd;
        if (!enabled) {
            fill = p.theme.surface;
            txt = p.theme.text_disabled;
            bd = p.theme.border_subtle;
        } else if (variant == UI_BUTTON_PRIMARY) {
            fill = pressed ? p.theme.accent_pressed
                 : hover   ? p.theme.accent_hover
                           : p.theme.accent;
            txt = p.theme.accent_contrast;
            bd = p.theme.accent_pressed;
        } else if (variant == UI_BUTTON_DANGER) {
            color_t d = p.theme.danger;
            fill = pressed ? color_darken(d, 40)
                 : hover   ? color_lighten(d, 30)
                           : d;
            txt = color_get_contrast_text(d);
            bd = color_darken(d, 40);
        } else {
            color_t base = p.theme.surface_elevated;
            fill = pressed ? color_darken(base, 30)
                 : hover   ? theme_mix(p.theme.text, base, 28)
                           : base;
            txt = p.theme.text;
            bd = p.theme.border;
        }
        // Focus ring = border 1px menjadi warna focus (di dalam bounds agar
        // damage tracking tetap tepat — tidak menggambar di luar rect).
        if (enabled && has_focus) bd = p.theme.focus;
        p.rrect(x, y, w, h, 6, fill);
        p.rrect_border(x, y, w, h, 6, bd, 255);
        // Inset shadow tipis di tepi atas saat ditekan.
        if (enabled && pressed) p.blend_rect(x + 6, y + 1, w - 12, 1, COLOR_BLACK, 60);
        p.text(text, x + (w - _ui_strlen(text) * 8) / 2,
               y + (h - 16) / 2 + ((enabled && pressed) ? 1 : 0), txt);
    }
    virtual void set_hover(bool on) override { hover = on; if (!on) pressed = false; mark_dirty(); }
    virtual void on_click(int mx, int my) override {
        if (!enabled) return;
        pressed = true;             // render() dipanggil Window setelah ini
        mark_dirty();
        Widget::on_click(mx, my);
    }
    virtual void on_release() override { pressed = false; mark_dirty(); }
    // Keyboard: Enter/Spasi mengaktifkan tombol fokus (gaya native).
    // Shortcut registry Window dicek lebih dulu, jadi tidak ada rebutan.
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        (void)mods;
        if (!enabled) return;
        uint32_t sc = scancode & 0xFF;
        if (ascii == '\n' || ascii == '\r' || ascii == ' ' || sc == 0x1C) {
            mark_dirty();
            Widget::on_click(0, 0);
        }
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_BUTTON_HPP
