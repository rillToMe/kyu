// libs/widget/include/chrome/statusbar.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CHROME_STATUSBAR_HPP
#define KWIDGET_CHROME_STATUSBAR_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// StatusBar — pita status di dasar window (gaya Notepad Windows):
// garis pemisah 1px di atas, teks kiri (Ln/Col) + teks kanan (info dokumen).
// ------------------------------------------------------------
class StatusBar : public Widget {
public:
    char* left;
    char* right;

    StatusBar() : left(0), right(0) { w = 0; h = 20; }
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
        // Status bar = permukaan "chrome" (senada menubar) + divider 1px di
        // atasnya sebagai batas dari area teks.
        p.rect(x, y, w, h, p.theme.chrome);
        p.rect(x, y, w, 1, p.theme.divider);
        if (left) p.text(left, x + 8, y + (h - 16) / 2, p.theme.button_fg);
        if (right) {
            int tw = _ui_strlen(right) * 8;
            p.text(right, x + w - tw - 8, y + (h - 16) / 2, p.theme.button_fg);
        }
    }
};

} // namespace ui

#endif // KWIDGET_CHROME_STATUSBAR_HPP
