// libs/widget/include/primitives/switch.hpp — Switch (toggle on/off).
//
// KAPAN SWITCH, KAPAN CHECKBOX
// Checkbox = pilihan yang bagian dari sebuah form (dikirim bersama tombol).
// Switch  = pengaturan yang BERLAKU SEGERA (nyala/mati tanpa tombol simpan).
// Itu sebabnya keduanya ada, dan itu sebabnya Switch memakai bentuk yang
// jelas berbeda (track + knob) alih-alih kotak centang berwarna.
//
// Geometri: track 32×18, knob 14px. Ukuran kecil ini disengaja — switch yang
// besar mendominasi baris dan membuat daftar pengaturan terasa berat. Track
// memakai radius PILL karena bentuknya memang kapsul.
#ifndef KWIDGET_PRIMITIVES_SWITCH_HPP
#define KWIDGET_PRIMITIVES_SWITCH_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/state.hpp"

namespace ui {

class Switch : public Widget {
public:
    char* label;
    bool on;
    bool hover;
    bool pressed;
    ui_click_cb change_cb;
    void* change_data;

    enum { TRACK_W = 32, TRACK_H = 18, KNOB = 14 };

    Switch(const char* t)
        : label(_ui_strdup(t)), on(false), hover(false), pressed(false),
          change_cb(0), change_data(0) {
        const Metrics m;
        h = m.control_h_sm;
        int tw = label ? text_measure_role(label, Typography().label) : 0;
        w = TRACK_W + (tw ? m.sm + tw : 0);
        cursor_kind = UI_CURSOR_HAND;
    }
    virtual ~Switch() { _ui_free(label); }
    void set_on(bool v, bool fire) {
        if (on == v) return;
        on = v;
        mark_dirty();
        if (fire && change_cb) change_cb(change_data);
    }
    bool is_on() const { return on; }
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }

    virtual bool focusable() override { return enabled; }
    virtual void set_hover(bool onv) override {
        if (hover == onv) return;
        hover = onv;
        if (!onv) pressed = false;
        mark_dirty();
    }
    virtual void on_click(int mx, int my) override {
        (void)mx; (void)my;
        if (!enabled) return;
        pressed = true;
        set_on(!on, true);
    }
    virtual void on_release() override {
        if (!pressed) return;
        pressed = false;
        mark_dirty();
    }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        (void)mods;
        if (!enabled) return;
        uint32_t sc = scancode & 0xFF;
        // Spasi/Enter toggle; panah kiri/kanan juga (native: switch adalah
        // kontrol dua-nilai, jadi arah harus terasa).
        if (ascii == '\n' || ascii == '\r' || ascii == ' ' || sc == 0x1C) {
            set_on(!on, true);
        } else if (sc == 0x4B) {          // Left
            if (on) set_on(false, true);
        } else if (sc == 0x4D) {          // Right
            if (!on) set_on(true, true);
        }
    }
    virtual void draw(Painter& p) override {
        const Metrics& m = p.theme.metrics;
        const TypeRole& role = p.theme.type.body;
        StateInputs st;
        st.hover = hover;
        st.pressed = pressed;
        st.focused = has_focus;
        st.enabled = enabled;

        const int ty = y + (h - TRACK_H) / 2;
        color_t track = !enabled ? p.theme.surface_variant
                      : on       ? (pressed ? p.theme.accent_pressed
                                   : hover  ? p.theme.accent_hover
                                            : p.theme.accent)
                                 : (pressed ? p.theme.surface_pressed
                                   : hover   ? p.theme.surface_hover
                                             : p.theme.surface_variant);
        p.surface(x, ty, TRACK_W, TRACK_H, track, radius::PILL);
        // Border tipis saat OFF: tanpa itu track gelap lenyap di latar gelap.
        if (!on) p.rrect_border(x, ty, TRACK_W, TRACK_H, radius::PILL,
                                has_focus ? p.theme.focus : p.theme.border, 255);
        else if (has_focus) p.rrect_border(x, ty, TRACK_W, TRACK_H, radius::PILL,
                                           p.theme.focus, 255);

        // Knob: permukaan kontras, digeser 2px dari tepi.
        int kx = on ? x + TRACK_W - KNOB - 2 : x + 2;
        int ky = ty + (TRACK_H - KNOB) / 2;
        color_t knob = !enabled ? p.theme.text_disabled : p.theme.surface;
        if (on && enabled) knob = p.theme.accent_contrast;
        p.surface(kx, ky, KNOB, KNOB, knob, radius::PILL);

        if (label && label[0]) {
            int tx = x + TRACK_W + m.sm;
            p.text_ellipsis(label, tx, y + text_vcenter(h), w - TRACK_W - m.sm,
                            state_text(p.theme, st), role);
        }
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_SWITCH_HPP
