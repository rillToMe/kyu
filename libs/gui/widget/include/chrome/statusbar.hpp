// libs/widget/include/chrome/statusbar.hpp — pita status di dasar window.
//
// Statusbar = pita chrome (sama dengan menubar) dengan garis 1px di ATASNYA
// sebagai batas dari area isi. Isinya selalu dua kolom: teks kiri = posisi
// (primer), teks kanan = info dokumen (sekunder). Hierarki lewat tone, bukan
// lewat warna khusus.
#ifndef KWIDGET_CHROME_STATUSBAR_HPP
#define KWIDGET_CHROME_STATUSBAR_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

class StatusBar : public Widget {
public:
    char* left;
    char* right;

    StatusBar() : left(0), right(0) {
        w = 0;
        h = chrome::STATUSBAR_H;
    }
    virtual ~StatusBar() { _ui_free(left); _ui_free(right); }

    void set_text(const char* l, const char* r) {
        char* nl = _ui_strdup(l ? l : "");
        if (!nl) return;
        char* nr = (r && r[0]) ? _ui_strdup(r) : 0;   // 0 = kolom kanan kosong
        mark_dirty();
        _ui_free(left); _ui_free(right);
        left = nl; right = nr;
        mark_dirty();
    }
    virtual void draw(Painter& p) override {
        const Metrics& m = p.theme.metrics;
        const TypeRole& role = p.theme.type.caption;
        p.rect(x, y, w, h, p.theme.chrome);
        p.rect(x, y, w, 1, p.theme.border_subtle);
        int ty = y + text_vcenter(h);
        if (left) {
            p.text_ellipsis(left, x + m.sm, ty, w - 2 * m.sm, p.theme.text_secondary,
                            role);
        }
        if (right) {
            int tw = text_measure(right);
            p.text(right, x + w - tw - m.sm, ty, p.theme.text_tertiary);
        }
    }
};

} // namespace ui

#endif // KWIDGET_CHROME_STATUSBAR_HPP
