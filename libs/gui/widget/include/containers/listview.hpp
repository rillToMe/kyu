// libs/widget/include/containers/listview.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CONTAINERS_LISTVIEW_HPP
#define KWIDGET_CONTAINERS_LISTVIEW_HPP

#include "containers/scrollable.hpp"

namespace ui {

// ------------------------------------------------------------
// ListView — daftar item vertikal, row 20px, pilih + scroll.
// ------------------------------------------------------------
class ListView : public Scrollable {
public:
    enum { MAX_ITEMS = 32 };
    char* items[MAX_ITEMS];
    int n;
    int selected, hover_row;
    ui_click_cb change_cb;
    void* change_data;

    ListView(int width, int height) : n(0), selected(-1), hover_row(-1),
                                      change_cb(0), change_data(0) {
        w = width; h = height;
        for (int i = 0; i < MAX_ITEMS; i++) items[i] = 0;
        set_scroll_max(0);
    }
    virtual ~ListView() { for (int i = 0; i < n; i++) _ui_free(items[i]); }
    void add_item(const char* label) {
        if (n >= MAX_ITEMS) return;
        items[n++] = _ui_strdup(label);
        set_scroll_max(n * ROW_H);
        mark_dirty();
    }
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }
    // Phase 11: pilih baris dari kode (viewer membuka berkas dari Explorer)
    // + gulirkan baris itu ke dalam view kalau sedang di luar. Tidak memanggil
    // change_cb — pemanggilnya yang tahu dan menghindari rekursi.
    void set_selected(int i) {
        if (i < 0 || i >= n || i == selected) return;
        selected = i;
        int view_h = h - BAR_W;
        int ry = i * ROW_H;
        if (ry < scroll) scroll = ry;
        else if (ry + ROW_H > scroll + view_h) scroll = ry + ROW_H - view_h;
        if (scroll < 0) scroll = 0;
        if (scroll > scroll_max) scroll = scroll_max;
        mark_dirty();
    }
    virtual void set_hover(bool on) override { if (!on && hover_row >= 0) { hover_row = -1; mark_dirty(); } }
    virtual bool track_hover(int mx, int my) override {
        (void)mx;
        int r = -1;
        if (my >= y && my < y + h) {
            r = (scroll + my - y) / ROW_H;
            if (r < 0 || r >= n) r = -1;
        }
        if (r == hover_row) return false;
        hover_row = r;
        mark_dirty();
        return true;
    }
    virtual void on_content_click(int mx, int my) override {
        (void)mx;
        int r = (scroll + my - y) / ROW_H;
        if (r >= 0 && r < n) {
            selected = r;
            mark_dirty();
            if (change_cb) change_cb(change_data);
        }
    }
    virtual void draw(Painter& p) override {
        int cw = content_w();
        p.set_clip(x, y, cw, h);
        for (int i = 0; i < n; i++) {
            int ry = y + i * ROW_H - scroll;
            if (ry + ROW_H <= y || ry >= y + h) continue;
            if (i == selected) p.rect(x, ry, cw, ROW_H, p.theme.button_bg);
            else if (i == hover_row) p.rect(x, ry, cw, ROW_H, p.theme.button_hover);
            p.text(items[i], x + 4, ry + 2, p.theme.fg);
        }
        p.clear_clip();
        draw_bar(p);
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_LISTVIEW_HPP
