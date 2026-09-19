// libs/widget/include/chrome/menubar.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CHROME_MENUBAR_HPP
#define KWIDGET_CHROME_MENUBAR_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "chrome/menu.hpp"

namespace ui {

class Window;   // fwd: MenuBar pegang Window* — TIDAK include window.hpp

class MenuBar : public Widget {
public:
    struct Title { char* label; Menu* menu; };
    enum { MAX_TITLES = 8 };
    Title titles[MAX_TITLES];
    int n, hover_idx;
    Window* win;

    MenuBar(Window* w, int width) : n(0), hover_idx(-1), win(w) {
        this->w = width; h = 24;
    }
    virtual ~MenuBar() {
        for (int i = 0; i < n; i++) { _ui_free(titles[i].label); delete titles[i].menu; }
    }
    Menu* add_menu(const char* title) {
        if (n >= MAX_TITLES) return 0;
        titles[n].label = _ui_strdup(title);
        titles[n].menu = new Menu(win);
        n++;
        return titles[n - 1].menu;
    }
    virtual bool is_menu_bar() override { return true; }
    // Lebar judul mengikuti teksnya (gaya menu Windows), TIDAK dibagi rata
    // selebar window — dulu judul berjarak lebar sehingga terlihat seperti
    // tabel, bukan menu.
    int title_w(int i) const { return _ui_strlen(titles[i].label) * 8 + 22; }
    int title_x(int i) const {
        int tx = x + 6;
        for (int j = 0; j < i; j++) tx += title_w(j);
        return tx;
    }
    void mark_title(int i) {
        if (i < 0 || i >= n) return;
        mark_area(title_x(i), y, title_w(i), h);
    }
    virtual void set_hover(bool on) override { if (!on && hover_idx >= 0) { mark_title(hover_idx); hover_idx = -1; } }
    int title_at(int mx) const {
        for (int i = 0; i < n; i++) {
            int tx = title_x(i);
            if (mx >= tx && mx < tx + title_w(i)) return i;
        }
        return -1;
    }
    virtual bool track_hover(int mx, int my) override {
        int i = -1;
        if (my >= y && my < y + h) i = title_at(mx);
        if (i == hover_idx) return false;
        mark_title(hover_idx);
        hover_idx = i;
        mark_title(hover_idx);
        return true;
    }
    // out-of-class: butuh Window lengkap (popup switch/open/close)
    virtual void on_click(int mx, int my) override;
    virtual void draw(Painter& p) override;
};

} // namespace ui

#endif // KWIDGET_CHROME_MENUBAR_HPP
