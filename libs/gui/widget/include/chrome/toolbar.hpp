// libs/widget/include/chrome/toolbar.hpp — bar tombol di bawah menubar.
//
// Toolbar adalah permukaan chrome yang sama dengan menubar (satu pita visual),
// dengan tombol-tombol di dalamnya. Tombolnya BUKAN tombol penuh (tidak ada
// border): di dalam toolbar, tombol adalah "aksi tenang" yang baru terlihat
// saat hover — supaya toolbar tidak menjadi deretan kotak.
//
// Tombol bisa memakai IKON (sistem ikon) alih-alih teks; ikon dan teks
// memakai ukuran baris yang sama sehingga toolbar tetap satu garis dasar.
#ifndef KWIDGET_CHROME_TOOLBAR_HPP
#define KWIDGET_CHROME_TOOLBAR_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

class Toolbar : public Widget {
public:
    struct Btn {
        char* label;
        int icon;           // ICON_* (0 = teks saja)
        ui_click_cb cb;
        void* data;
    };
    enum { MAX_BTNS = 16 };
    Btn btns[MAX_BTNS];
    int n, hover_idx;

    Toolbar(int width) : n(0), hover_idx(-1) {
        w = width; h = chrome::TOOLBAR_H;
    }
    virtual ~Toolbar() { for (int i = 0; i < n; i++) _ui_free(btns[i].label); }
    void add_button(const char* label, ui_click_cb cb, void* u) {
        add_button_icon(label, ICON_NONE, cb, u);
    }
    // Tombol ber-ikon. label boleh "" (tombol ikon saja).
    void add_button_icon(const char* label, int icon, ui_click_cb cb, void* u) {
        if (n >= MAX_BTNS) return;
        btns[n].label = _ui_strdup(label ? label : "");
        btns[n].icon = (icon > ICON_NONE && icon < ICON_COUNT) ? icon : ICON_NONE;
        btns[n].cb = cb; btns[n].data = u;
        n++;
    }
    // Lebar tombol mengikuti ISI (ikon + teks), bukan dibagi rata: tombol
    // "Buka" tidak boleh selebar tombol "Simpan Sebagai".
    int btn_w(int i) const {
        const Metrics m;
        int tw = _ui_strlen(btns[i].label) * glyph::ADVANCE;
        int iw = (btns[i].icon != ICON_NONE) ? m.icon_md : 0;
        int gap = (iw && tw) ? m.sm : 0;
        return iw + gap + tw + 2 * m.sm;
    }
    int btn_x(int i) const {
        int bx = x + space::XS;
        for (int j = 0; j < i; j++) bx += btn_w(j) + space::XS;
        return bx;
    }
    int btn_at(int mx) const {
        for (int i = 0; i < n; i++) {
            int bx = btn_x(i);
            if (mx >= bx && mx < bx + btn_w(i)) return i;
        }
        return -1;
    }
    void mark_item(int i) {
        if (i < 0 || i >= n) return;
        mark_area(btn_x(i), y, btn_w(i), h);
    }
    virtual void set_hover(bool onv) override {
        if (!onv && hover_idx >= 0) { mark_item(hover_idx); hover_idx = -1; }
    }
    virtual bool track_hover(int mx, int my) override {
        int i = -1;
        if (my >= y && my < y + h && n) i = btn_at(mx);
        if (i == hover_idx) return false;
        mark_item(hover_idx);
        hover_idx = i;
        mark_item(hover_idx);
        return true;
    }
    virtual void on_click(int mx, int my) override {
        if (!n || my < y || my >= y + h) return;
        int i = btn_at(mx);
        if (i >= 0 && i < n && btns[i].cb) btns[i].cb(btns[i].data);
    }
    virtual void draw(Painter& p) override {
        const Metrics& m = p.theme.metrics;
        const TypeRole& role = p.theme.type.label;
        // Satu pita dengan menubar + garis 1px di bawah.
        p.rect(x, y, w, h, p.theme.chrome);
        p.rect(x, y + h - 1, w, 1, p.theme.border_subtle);
        for (int i = 0; i < n; i++) {
            int bx = btn_x(i), bw = btn_w(i);
            bool hov = (i == hover_idx);
            if (hov) {
                p.surface(bx, y + space::XS, bw, h - 2 * space::XS,
                          p.theme.surface_hover, m.radius_control);
            }
            int tw = _ui_strlen(btns[i].label) * glyph::ADVANCE;
            int iw = (btns[i].icon != ICON_NONE) ? m.icon_md : 0;
            int gap = (iw && tw) ? m.sm : 0;
            int cx = bx + (bw - (iw + gap + tw)) / 2;
            color_t tc = hov ? p.theme.text : p.theme.text_secondary;
            if (iw) {
                p.icon(btns[i].icon, cx + iw / 2, y + h / 2, iw, tc);
                cx += iw + gap;
            }
            if (tw) p.text_role(btns[i].label, cx, y + text_vcenter(h), tc, role);
        }
    }
};

} // namespace ui

#endif // KWIDGET_CHROME_TOOLBAR_HPP
