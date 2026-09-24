// libs/widget/include/containers/tab.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CONTAINERS_TAB_HPP
#define KWIDGET_CONTAINERS_TAB_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// Tab — strip tab 26px + panel aktif. Panel dimiliki (didelete)
// oleh Tab. Area panel = (y+26, w × h-26), di-clip.
// ------------------------------------------------------------
class Tab : public Widget {
public:
    enum { MAX_TABS = 8, STRIP_H = 26 };
    char* titles[MAX_TABS];
    Widget* panels[MAX_TABS];
    int n, active;

    Tab(int width, int height) : n(0), active(0) {
        w = width; h = height;
        for (int i = 0; i < MAX_TABS; i++) { titles[i] = 0; panels[i] = 0; }
    }
    virtual ~Tab() {
        for (int i = 0; i < n; i++) { _ui_free(titles[i]); delete panels[i]; }
    }
    void add(const char* title, Widget* panel) {
        if (n >= MAX_TABS) return;
        titles[n] = _ui_strdup(title);
        panels[n] = panel;
        // Panel BUKAN anggota children[] jadi owner tak mengalir otomatis:
        // teruskan eksplisit (lihat ScrollView::set_child).
        if (panel) panel->set_owner(owner);
        n++;
        mark_dirty();
    }
    // Propagasi owner susulan (ui_window_add terakhir setelah build).
    virtual void set_owner(Window* o) override {
        Widget::set_owner(o);
        for (int i = 0; i < n; i++)
            if (panels[i]) panels[i]->set_owner(o);
    }
    virtual int dirty_child_count() override { return n; }
    virtual Widget* dirty_child(int i) override { return panels[i]; }
    int title_at(int mx) const {
        if (n == 0 || mx < x || mx >= x + w) return -1;
        int tw = w / n;
        int i = (mx - x) / tw;
        return (i >= 0 && i < n) ? i : -1;
    }
    virtual Widget* pick(int mx, int my) override {
        if (!visible) return 0;
        if (my >= y && my < y + STRIP_H)
            return (mx >= x && mx < x + w) ? this : 0;
        if (active < n && panels[active])
            return panels[active]->pick(mx, my);
        return 0;
    }
    virtual void on_click(int mx, int my) override {
        if (my >= y && my < y + STRIP_H) {
            int i = title_at(mx);
            if (i >= 0 && i != active) { active = i; mark_dirty(); }
            return;
        }
        if (active < n && panels[active]) {
            Widget* c = panels[active]->pick(mx, my);
            if (c) c->on_click(mx, my);
        }
    }
    virtual void draw(Painter& p) override {
        int tw = n ? w / n : w;
        for (int i = 0; i < n; i++) {
            int tx = x + i * tw;
            bool act = (i == active);
            p.rect(tx, y, tw, STRIP_H, act ? p.theme.button_bg : p.theme.bg);
            if (act) p.rect(tx, y + STRIP_H - 2, tw, 2, p.theme.accent);
            int tl = _ui_strlen(titles[i]) * 8;
            p.text(titles[i], tx + (tw - tl) / 2, y + (STRIP_H - 16) / 2, p.theme.fg);
        }
        if (active < n && panels[active]) {
            Widget* pl = panels[active];
            pl->x = x; pl->y = y + STRIP_H; pl->w = w; pl->h = h - STRIP_H;
            p.set_clip(x, y + STRIP_H, w, h - STRIP_H);
            pl->draw(p);
            p.clear_clip();
        }
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_TAB_HPP
