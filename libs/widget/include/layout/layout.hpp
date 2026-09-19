// libs/widget/include/layout/layout.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_LAYOUT_LAYOUT_HPP
#define KWIDGET_LAYOUT_LAYOUT_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// Layout — kontainer widget; hit-test anak topmost-first
// ------------------------------------------------------------
class Layout : public Widget {
public:
    enum { MAX_CHILDREN = 16 };
    Widget* children[MAX_CHILDREN];
    int count;

    Layout() : count(0) { for (int i = 0; i < MAX_CHILDREN; i++) children[i] = 0; }
    virtual ~Layout() { for (int i = 0; i < count; i++) delete children[i]; }

    void add(Widget* c) { if (count < MAX_CHILDREN) { c->set_owner(owner); children[count++] = c; } }
    virtual void arrange() = 0;
    // Phase 8: pindahkan anak; tandai posisi lama+baru bila berubah (reflow).
    void place(Widget* c, int nx, int ny) {
        if (c->x != nx || c->y != ny) {
            c->mark_area(c->x, c->y, c->w, c->h);
            c->x = nx; c->y = ny;
            c->mark_area(c->x, c->y, c->w, c->h);
        }
    }
    virtual int dirty_child_count() override { return count; }
    virtual Widget* dirty_child(int i) override { return children[i]; }
    virtual void settle() override { arrange(); }
    virtual void set_owner(Window* o) override {
        Widget::set_owner(o);
        for (int i = 0; i < count; i++) children[i]->set_owner(o);
    }

    virtual void draw(Painter& p) override {
        arrange();
        // Anak TERSEMBUNYI tidak digambar. arrange() sudah melewatinya saat
        // menempatkan, jadi posisinya bisa kebetulan (0,0) — persis bug yang
        // membuat bar cari Notepad menumpuk menubar sebelum ini.
        for (int i = 0; i < count; i++)
            if (children[i]->visible) children[i]->draw(p);
    }
    virtual Widget* pick(int mx, int my) override {
        if (!visible) return 0;
        for (int i = count - 1; i >= 0; i--) {
            Widget* r = children[i]->pick(mx, my);
            if (r) return r;
        }
        return 0;
    }
    virtual void collect_bounds(int& x0, int& y0, int& x1, int& y1) override {
        arrange();   // posisi anak dihitung di sini
        for (int i = 0; i < count; i++) children[i]->collect_bounds(x0, y0, x1, y1);
    }
};

} // namespace ui

#endif // KWIDGET_LAYOUT_LAYOUT_HPP
