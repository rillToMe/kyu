// libs/widget/include/containers/treeview.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CONTAINERS_TREEVIEW_HPP
#define KWIDGET_CONTAINERS_TREEVIEW_HPP

#include "containers/scrollable.hpp"

namespace ui {

// ------------------------------------------------------------
// TreeView — node ber-indent depth*12, marker '+'/'-' untuk
// expand/collapse (font 8x16 tanpa segitiga), pilih node.
// ------------------------------------------------------------
class TreeView : public Scrollable {
public:
    struct Node { char* label; int depth; bool expanded; };
    enum { MAX_NODES = 32 };
    Node nodes[MAX_NODES];
    int n;
    int selected, hover_row;
    ui_click_cb change_cb;
    void* change_data;

    TreeView(int width, int height) : n(0), selected(-1), hover_row(-1),
                                      change_cb(0), change_data(0) {
        w = width; h = height;
        set_scroll_max(0);
    }
    virtual ~TreeView() { for (int i = 0; i < n; i++) _ui_free(nodes[i].label); }
    void add_node(const char* label, int depth, int expanded) {
        if (n >= MAX_NODES) return;
        nodes[n].label = _ui_strdup(label);
        nodes[n].depth = depth;
        nodes[n].expanded = expanded;
        n++;
        recompute_scroll();
        mark_dirty();
    }
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }
    virtual void set_hover(bool on) override { if (!on && hover_row >= 0) { hover_row = -1; mark_dirty(); } }

    // Node i terlihat bila ancestor terdekatnya (node j<i depth lebih kecil)
    // sedang expanded. Flat pre-order.
    bool visible_node(int i) const {
        for (int j = i - 1; j >= 0; j--)
            if (nodes[j].depth < nodes[i].depth) return nodes[j].expanded;
        return true;
    }
    bool has_children(int i) const {
        for (int j = i + 1; j < n; j++) {
            if (nodes[j].depth <= nodes[i].depth) return false;
            if (nodes[j].depth == nodes[i].depth + 1) return true;
        }
        return false;
    }
    void recompute_scroll() {
        int vis = 0;
        for (int i = 0; i < n; i++) if (visible_node(i)) vis++;
        set_scroll_max(vis * ROW_H);
    }
    // Map vis_row (baris tampil ke-N) -> index node, atau -1.
    int node_at_vis(int vis_row) const {
        int v = 0;
        for (int i = 0; i < n; i++) {
            if (!visible_node(i)) continue;
            if (v == vis_row) return i;
            v++;
        }
        return -1;
    }
    virtual bool track_hover(int mx, int my) override {
        (void)mx;
        int r = -1;
        if (my >= y && my < y + h)
            r = node_at_vis((scroll + my - y) / ROW_H);
        if (r == hover_row) return false;
        hover_row = r;
        mark_dirty();
        return true;
    }
    virtual void on_content_click(int mx, int my) override {
        int idx = node_at_vis((scroll + my - y) / ROW_H);
        if (idx < 0) return;
        int ind = x + nodes[idx].depth * 12;
        if (mx >= ind && mx < ind + 12) {
            nodes[idx].expanded = !nodes[idx].expanded;
            recompute_scroll();
            mark_dirty();
        } else if (mx >= ind + 12) {
            selected = idx;
            mark_dirty();
            if (change_cb) change_cb(change_data);
        }
    }
    virtual void draw(Painter& p) override {
        int cw = content_w();
        p.set_clip(x, y, cw, h);
        int vis = 0;
        for (int i = 0; i < n; i++) {
            if (!visible_node(i)) continue;
            int ry = y + vis * ROW_H - scroll;
            vis++;
            if (ry + ROW_H <= y || ry >= y + h) continue;
            if (i == selected) p.rect(x, ry, cw, ROW_H, p.theme.button_bg);
            else if (i == hover_row) p.rect(x, ry, cw, ROW_H, p.theme.button_hover);
            int ind = x + nodes[i].depth * 12;
            if (has_children(i))
                p.text(nodes[i].expanded ? "-" : "+", ind, ry + 2, p.theme.accent);
            p.text(nodes[i].label, ind + 12, ry + 2, p.theme.fg);
        }
        p.clear_clip();
        draw_bar(p);
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_TREEVIEW_HPP
