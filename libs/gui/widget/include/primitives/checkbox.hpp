// libs/widget/include/primitives/checkbox.hpp — kotak centang + label.
//
// Bahasa visual: kotak 16px (radius SMALL) sejajar dengan glyph teks, dengan
// indikator centang dari SISTEM IKON (bukan gambar ad-hoc), sehingga bentuk
// centang identik dengan ikon lain di seluruh UI.
//
// Barisnya menyediakan SELURUH area klik (kotak + label), bukan hanya kotaknya:
// target klik kecil adalah salah satu keluhan paling umum pada UI desktop.
#ifndef KWIDGET_PRIMITIVES_CHECKBOX_HPP
#define KWIDGET_PRIMITIVES_CHECKBOX_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/state.hpp"

namespace ui {

class CheckBox : public Widget {
public:
    char* label;
    bool checked;
    bool hover;
    ui_click_cb toggle_cb;
    void* toggle_data;

    CheckBox(const char* t)
        : label(_ui_strdup(t)), checked(false), hover(false),
          toggle_cb(0), toggle_data(0) {
        const Metrics m;
        h = m.control_h_sm;
        w = text_width(label, Typography().label) + m.control_h_sm + m.sm;
    }
    virtual ~CheckBox() { _ui_free(label); }
    void set_checked(bool c) {
        if (checked == c) return;
        checked = c;
        mark_dirty();
    }
    virtual bool focusable() override { return enabled; }
    virtual void set_hover(bool on) override {
        if (hover == on) return;
        hover = on;
        mark_dirty();
    }
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
        const Metrics& m = p.theme.metrics;
        const TypeRole& role = p.theme.type.body;
        StateInputs st;
        st.hover = hover;
        st.focused = has_focus;
        st.enabled = enabled;

        const int box = m.icon_md;                    // 16px — sejajar glyph
        const int by = y + (h - box) / 2;
        const int r = m.radius_small;

        color_t fill = !enabled  ? p.theme.surface_variant
                     : checked   ? p.theme.accent
                     : st.hover  ? p.theme.surface_hover
                                 : p.theme.surface;
        // Border: checked → senada isi (tepi tetap tegas tanpa garis kontras);
        // fokus → warna fokus; invalid ditangani pemanggil lewat set_error.
        color_t bd = !enabled ? p.theme.border_subtle
                   : checked  ? fill
                   : has_focus ? p.theme.focus
                   : hover    ? p.theme.text_secondary
                              : p.theme.border;
        p.surface(x, by, box, box, fill, r);
        p.rrect_border(x, by, box, box, r, bd, 255);
        if (checked) {
            color_t mk = enabled ? p.theme.accent_contrast : p.theme.text_disabled;
            p.icon(ICON_CHECK, x + box / 2, by + box / 2, box, mk);
        }
        if (label && label[0]) {
            int tx = x + box + m.sm;
            int avail = w - box - m.sm;
            p.text_ellipsis(label, tx, y + text_vcenter(h),
                            avail, state_text(p.theme, st), role);
        }
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_CHECKBOX_HPP
