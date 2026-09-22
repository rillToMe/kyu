// libs/widget/include/chrome/toolbar.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CHROME_TOOLBAR_HPP
#define KWIDGET_CHROME_TOOLBAR_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// Toolbar — bar tombol full-width (ditaruh via Window::add_bar).
// ------------------------------------------------------------
class Toolbar : public Widget {
public:
    struct Btn { char* label; ui_click_cb cb; void* data; };
    enum { MAX_BTNS = 16 };
    Btn btns[MAX_BTNS];
    int n, hover_idx;

    Toolbar(int width) : n(0), hover_idx(-1) {
        w = width; h = 28;
    }
    virtual ~Toolbar() { for (int i = 0; i < n; i++) _ui_free(btns[i].label); }
    void add_button(const char* label, ui_click_cb cb, void* u) {
        if (n >= MAX_BTNS) return;
        btns[n].label = _ui_strdup(label);
        btns[n].cb = cb; btns[n].data = u;
        n++;
    }
    void mark_item(int i) {
        if (i < 0 || i >= n) return;
        int bw = n ? w / n : w;
        mark_area(x + i * bw, y, bw, h);
    }
    virtual void set_hover(bool on) override { if (!on && hover_idx >= 0) { mark_item(hover_idx); hover_idx = -1; } }
    virtual bool track_hover(int mx, int my) override {
        int i = -1;
        if (my >= y && my < y + h && n) {
            i = (mx - x) / (w / n);
            if (i < 0 || i >= n) i = -1;
        }
        if (i == hover_idx) return false;
        mark_item(hover_idx);       // item lama
        hover_idx = i;
        mark_item(hover_idx);       // item baru
        return true;
    }
    virtual void on_click(int mx, int my) override {
        if (!n || my < y || my >= y + h) return;
        int i = (mx - x) / (w / n);
        if (i >= 0 && i < n && btns[i].cb) btns[i].cb(btns[i].data);
    }
    virtual void draw(Painter& p) override {
        int bw = n ? w / n : w;
        p.rect(x, y, w, h, p.theme.bg);
        for (int i = 0; i < n; i++) {
            int bx = x + i * bw;
            if (i == hover_idx) p.rect(bx + 2, y + 3, bw - 4, h - 6, p.theme.button_hover);
            int bl = _ui_strlen(btns[i].label) * 8;
            p.text(btns[i].label, bx + (bw - bl) / 2, y + (h - 16) / 2, p.theme.fg);
        }
    }
};

} // namespace ui

#endif // KWIDGET_CHROME_TOOLBAR_HPP
