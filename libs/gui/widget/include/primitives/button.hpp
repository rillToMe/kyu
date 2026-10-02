// libs/widget/include/primitives/button.hpp — tombol (variant semantik).
//
// BAHASA VISUAL TOMBOL KYUZENOS
// Tombol bukan "kotak membulat besar". Empat variant dengan peran jelas:
//
//   SECONDARY  permukaan + border 1px — aksi normal. Default.
//   PRIMARY    isi aksen — satu-satunya aksi utama pada sebuah permukaan.
//              Border senada isi supaya tepi tetap tegas tanpa garis kontras.
//   TERTIARY   tanpa isi dan tanpa border — aksi tenang (Batal, Lewati,
//              tautan tindakan). Hanya teks + hover yang sangat halus.
//              Ini yang mencegah UI penuh kotak.
//   DANGER     isi danger — aksi merusak (Hapus, Reset).
//
// Ditambah bentuk ICON (tombol persegi berisi ikon saja) lewat `set_icon`.
//
// State: normal / hover / pressed / focused / disabled — semuanya lewat
// `core/state.hpp`, jadi taksonomi ini sama untuk semua kontrol lain.
#ifndef KWIDGET_PRIMITIVES_BUTTON_HPP
#define KWIDGET_PRIMITIVES_BUTTON_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/theme.hpp"
#include "core/state.hpp"

namespace ui {

class Button : public Widget {
public:
    char* text;
    bool hover;
    bool pressed;
    int variant;
    int icon_id;        // ICON_* (0 = tanpa ikon)

    Button(const char* t)
        : text(_ui_strdup(t ? t : "")), hover(false), pressed(false),
          variant(UI_BUTTON_SECONDARY), icon_id(ICON_NONE) {
        cursor_kind = UI_CURSOR_HAND;
        apply_metrics();
    }
    virtual ~Button() { _ui_free(text); }

    void set_variant(int v) {
        variant = (v >= UI_BUTTON_SECONDARY && v <= UI_BUTTON_TERTIARY)
                      ? v : UI_BUTTON_SECONDARY;
        mark_dirty();
    }
    void set_icon(int id) {
        icon_id = (id > ICON_NONE && id < ICON_COUNT) ? id : ICON_NONE;
        apply_metrics();
        mark_dirty();
    }
    // Ukuran dari isi: teks + padding token, atau persegi untuk tombol ikon.
    // Memakai Metrics() langsung (bukan Theme(): palet tidak dibutuhkan untuk
    // geometri, dan membangun Theme penuh di sini hanya membuang kerja).
    void apply_metrics() {
        const Metrics m;
        h = m.control_h;
        int tw = text ? _ui_strlen(text) * glyph::ADVANCE : 0;
        if (icon_id != ICON_NONE) {
            if (tw > 0) w = m.control_pad_x + m.icon_md + m.sm + tw + m.control_pad_x;
            else        w = m.control_h;      // tombol ikon = persegi
        } else {
            w = tw + 2 * m.control_pad_x;
        }
    }

    virtual bool focusable() override { return enabled; }
    virtual void set_hover(bool on) override {
        hover = on;
        if (!on) pressed = false;
        mark_dirty();
    }
    virtual void draw(Painter& p) override {
        const Metrics& m = p.theme.metrics;
        const TypeRole& role = p.theme.type.label;
        StateInputs st;
        st.hover = hover;
        st.pressed = pressed;
        st.focused = has_focus;
        st.enabled = enabled;

        color_t fill, txt, bd;
        bool draw_border = true;
        if (variant == UI_BUTTON_PRIMARY) {
            fill = !enabled ? p.theme.surface
                 : pressed  ? p.theme.accent_pressed
                 : hover    ? p.theme.accent_hover
                            : p.theme.accent;
            txt = enabled ? p.theme.accent_contrast : p.theme.text_disabled;
            bd = enabled ? fill : p.theme.border_subtle;   // border senada isi
        } else if (variant == UI_BUTTON_DANGER) {
            color_t d = p.theme.danger;
            fill = !enabled ? p.theme.surface
                 : pressed  ? color_darken(d, 40)
                 : hover    ? color_lighten(d, 30)
                            : d;
            txt = enabled ? color_get_contrast_text(d) : p.theme.text_disabled;
            bd = enabled ? fill : p.theme.border_subtle;
        } else if (variant == UI_BUTTON_TERTIARY) {
            // Tanpa isi & tanpa border. Hover/pressed hanya menggelapkan
            // permukaan di belakangnya — bentuknya tetap terasa "teks aksi",
            // bukan kotak yang muncul tiba-tiba.
            fill = !enabled ? p.theme.bg
                 : pressed  ? p.theme.surface_pressed
                 : hover    ? p.theme.surface_hover
                            : p.theme.bg;
            txt = enabled ? p.theme.text_secondary : p.theme.text_disabled;
            bd = fill;
            draw_border = (hover || pressed) && enabled;
        } else {
            SurfaceStyle ss = state_style(p.theme, st);
            fill = ss.fill;
            bd = ss.border;
            txt = ss.text;
            // Fokus: border menjadi warna fokus (bentuk ring yang paling
            // tenang untuk tombol — tidak menambah elemen baru).
            if (enabled && has_focus) bd = p.theme.focus;
        }

        const int r = m.radius_control;
        p.surface(x, y, w, h, fill, r);
        if (draw_border) p.rrect_border(x, y, w, h, r, bd, 255);

        // Konten: [ikon] [teks], terpusat sebagai satu grup.
        int tw = _ui_strlen(text) * glyph::ADVANCE;
        int iw = (icon_id != ICON_NONE) ? m.icon_md : 0;
        int gap = (iw && tw) ? m.sm : 0;
        int total = iw + gap + tw;
        int cx = x + (w - total) / 2;
        int ty = y + text_vcenter(h) + ((enabled && pressed) ? 1 : 0);
        if (iw) {
            p.icon(icon_id, cx + iw / 2, y + h / 2 + ((enabled && pressed) ? 1 : 0),
                   iw, txt);
            cx += iw + gap;
        }
        if (tw) p.text_role(text, cx, ty, txt, role);
    }
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
