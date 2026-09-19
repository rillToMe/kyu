// libs/widget/include/containers/scrollable.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CONTAINERS_SCROLLABLE_HPP
#define KWIDGET_CONTAINERS_SCROLLABLE_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// Scrollable — basis widget yang bisa di-scroll roda + scrollbar.
// ScrollView/ListView/Table/TreeView menimpa on_content_click dan
// memotong draw()-nya ke area konten. Ponytail: scrollbar thumb
// proporsional sederhana (bukan hitung drag-ratio penuh).
// ------------------------------------------------------------
class Window;   // fwd: Menu/MenuBar pegang Window* untuk popup

class Scrollable : public Widget {
public:
    enum { BAR_W = 6, ROW_H = 20 };
    int scroll, scroll_max;
    bool bar_drag;
    int bar_grab_y, bar_grab_scroll;

    Scrollable() : scroll(0), scroll_max(0), bar_drag(false),
                   bar_grab_y(0), bar_grab_scroll(0) {}

    void set_scroll_view(int content_h, int view_h) {
        scroll_max = content_h - view_h;
        if (scroll_max < 0) scroll_max = 0;
        if (scroll > scroll_max) scroll = scroll_max;
    }
    void set_scroll_max(int content_h) { set_scroll_view(content_h, h - BAR_W); }
    bool bar_hit(int mx) const { return mx >= x + w - BAR_W && mx < x + w; }
    // Lebar konten = tanpa scrollbar bila bar tampil, penuh bila tidak.
    int content_w() const { return scroll_max > 0 ? w - BAR_W : w; }

    virtual bool on_scroll(int delta) override {
        int old = scroll;
        scroll += delta * ROW_H;
        if (scroll < 0) scroll = 0;
        if (scroll > scroll_max) scroll = scroll_max;
        if (scroll != old) mark_dirty();
        return scroll != old;
    }
    virtual void on_click(int mx, int my) override {
        if (bar_hit(mx)) {
            bar_drag = true;
            bar_grab_y = my;
            bar_grab_scroll = scroll;
            int th = bar_thumb_h();
            int range = h - th;
            if (range > 0) scroll = (my - y - th / 2) * scroll_max / range;
            if (scroll < 0) scroll = 0;
            if (scroll > scroll_max) scroll = scroll_max;
            mark_dirty();
        } else {
            on_content_click(mx, my);
        }
    }
    virtual bool on_drag(int mx, int my) override {
        (void)mx;
        if (!bar_drag) return false;
        int th = bar_thumb_h();
        int range = h - th;
        if (range <= 0) return true;
        scroll = bar_grab_scroll + (my - bar_grab_y) * scroll_max / range;
        if (scroll < 0) scroll = 0;
        if (scroll > scroll_max) scroll = scroll_max;
        mark_dirty();
        return true;
    }
    virtual void on_release() override { bar_drag = false; }
    // Hook klik area konten (subclass). Default fire click_cb.
    virtual void on_content_click(int mx, int my) {
        (void)mx; (void)my;
        if (click_cb) click_cb(userdata);
    }
    int bar_thumb_h() const {
        int content_h = scroll_max + (h - BAR_W);
        if (content_h <= 0) return h;
        int th = (h - BAR_W) * (h - BAR_W) / content_h;
        if (th < 8) th = 8;
        return th;
    }
    void draw_bar(Painter& p) {
        if (scroll_max <= 0) return;
        int bx = x + w - BAR_W;
        p.rect(bx, y, BAR_W, h, p.theme.button_bg);
        int th = bar_thumb_h();
        int range = h - th;
        int ty = range > 0 ? y + scroll * range / scroll_max : y;
        p.rect(bx, ty, BAR_W, th, p.theme.accent);
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_SCROLLABLE_HPP
