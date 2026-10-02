// libs/widget/include/containers/scrollable.hpp — basis widget scrollable.
//
// SCROLLBAR KYUZENOS
// Scrollbar adalah bagian dari sistem visual, bukan sisa renderer. Aturannya:
//   * Track TRANSPARAN terhadap latar (tidak ada "kolom abu-abu" di tepi
//     daftar) — hanya thumb yang terlihat, dan hanya kalau memang bisa digulir.
//   * Thumb memakai surface_variant saat diam, dan menguat saat hover/drag.
//     Warna AKSEN tidak dipakai: aksen disediakan untuk makna (aksi utama,
//     nilai), bukan untuk menandai "ini bisa digulir".
//   * Thumb berbentuk kapsul dan punya panjang minimum supaya selalu bisa
//     digenggam, bahkan untuk konten yang sangat panjang.
//
// Semua widget scrollable (ListView, Table, TreeView, GridView, ScrollView)
// mewarisi tampilan ini — itu sebabnya scrollbar terasa satu sistem.
#ifndef KWIDGET_CONTAINERS_SCROLLABLE_HPP
#define KWIDGET_CONTAINERS_SCROLLABLE_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/state.hpp"

namespace ui {

class Window;   // fwd: Menu/MenuBar pegang Window* untuk popup

class Scrollable : public Widget {
public:
    // BAR = lebar track; ROW_H = langkah roda/baris default (token daftar).
    enum { BAR_W = scrollbar::BAR, ROW_H = list::ROW_H };
    int scroll, scroll_max;
    bool bar_drag;
    int bar_grab_y, bar_grab_scroll;
    bool bar_hover;        // pointer di atas track/thumb (track menguat)

    Scrollable() : scroll(0), scroll_max(0), bar_drag(false),
                   bar_grab_y(0), bar_grab_scroll(0), bar_hover(false) {}

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
        int view = h - BAR_W;
        int content_h = scroll_max + view;
        if (content_h <= 0) return view;
        int th = view * view / content_h;
        if (th < scrollbar::THUMB_MIN) th = scrollbar::THUMB_MIN;
        if (th > view) th = view;
        return th;
    }
    // Track/thumb menguat saat pointer berada di atasnya.
    virtual bool track_hover(int mx, int my) override {
        (void)my;
        bool onv = scroll_max > 0 && bar_hit(mx);
        if (onv == bar_hover) return false;
        bar_hover = onv;
        mark_dirty();
        return true;
    }
    // Scrollbar: track tanpa latar, thumb kapsul dengan inset 2px.
    void draw_bar(Painter& p) {
        if (scroll_max <= 0) return;
        int bx = x + w - BAR_W;
        int inset = scrollbar::TRACK_INSET;
        int tw = BAR_W - 2 * inset;
        int view = h - BAR_W;
        int th = bar_thumb_h();
        int range = view - th;
        int ty = range > 0 ? y + scroll * range / scroll_max : y;
        color_t tc = (bar_hover || bar_drag) ? p.theme.text_secondary
                                             : p.theme.surface_variant;
        p.surface(bx + inset, ty + inset, tw, th - 2 * inset, tc, radius::PILL);
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_SCROLLABLE_HPP
