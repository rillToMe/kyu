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
    bool tab_disabled[MAX_TABS];   // Phase C: judul nonaktif (skip klik/panah)
    int n, active, hover_idx;

    Tab(int width, int height) : n(0), active(0), hover_idx(-1) {
        w = width; h = height;
        for (int i = 0; i < MAX_TABS; i++) {
            titles[i] = 0; panels[i] = 0; tab_disabled[i] = false;
        }
    }
    virtual ~Tab() {
        for (int i = 0; i < n; i++) { _ui_free(titles[i]); delete panels[i]; }
    }
    void add(const char* title, Widget* panel) {
        if (n >= MAX_TABS) return;
        titles[n] = _ui_strdup(title);
        panels[n] = panel;
        tab_disabled[n] = false;
        // Panel BUKAN anggota children[] jadi owner tak mengalir otomatis:
        // teruskan eksplisit (lihat ScrollView::set_child).
        if (panel) panel->set_owner(owner);
        n++;
        mark_dirty();
    }
    // Phase C: nonaktifkan judul tanpa mengubah arsitektur tab.
    void set_tab_enabled(int i, int on) {
        if (i < 0 || i >= n) return;
        tab_disabled[i] = (on == 0);
        mark_dirty();
    }    // Propagasi owner susulan (ui_window_add terakhir setelah build).
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
        if (!visible || !enabled) return 0;
        if (my >= y && my < y + STRIP_H)
            return (mx >= x && mx < x + w) ? this : 0;
        if (active < n && panels[active])
            return panels[active]->pick(mx, my);
        return 0;
    }
    virtual void on_click(int mx, int my) override {
        if (my >= y && my < y + STRIP_H) {
            int i = title_at(mx);
            // Judul disabled: tak bisa fokus/aktivasi (tetap di tab aktif).
            if (i >= 0 && i != active && !tab_disabled[i]) { active = i; mark_dirty(); }
            return;
        }
        if (active < n && panels[active]) {
            Widget* c = panels[active]->pick(mx, my);
            if (c) c->on_click(mx, my);
        }
    }
    // Phase C: strip bisa memegang fokus; panah pindah antar judul yang
    // enabled (native). Enter tidak perlu (panah langsung mengaktifkan).
    virtual bool focusable() override { return enabled; }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        (void)ascii; (void)mods;
        if (!enabled || n <= 0) return;
        uint32_t sc = scancode & 0xFF;
        int dir = 0;
        if (sc == 0x4B) dir = -1;        // Left
        else if (sc == 0x4D) dir = 1;    // Right
        else return;
        mark_dirty();
        for (int k = 0; k < n; k++) {
            active = (active + dir + n) % n;
            if (!tab_disabled[active]) { mark_dirty(); return; }
        }
        // Semua disabled: active kembali ke semula (loop penuh).
        mark_dirty();
    }
    // Phase B: hover judul (hierarki aktif/hover/inaktif).
    // Phase C: judul disabled tak di-hover (tak bisa fokus/aktivasi).
    virtual void set_hover(bool on) override {
        if (!on && hover_idx >= 0) { mark_title(hover_idx); hover_idx = -1; }
    }
    virtual bool track_hover(int mx, int my) override {
        int i = -1;
        if (my >= y && my < y + STRIP_H) {
            i = title_at(mx);
            if (i >= 0 && tab_disabled[i]) i = -1;
        }
        if (i == hover_idx) return false;
        mark_title(hover_idx);
        hover_idx = i;
        mark_title(hover_idx);
        return true;
    }
    void mark_title(int i) {
        if (i < 0 || i >= n) return;
        int tw = n ? w / n : w;
        mark_area(x + i * tw, y, tw, STRIP_H);
    }
    virtual void draw(Painter& p) override {
        int tw = n ? w / n : w;
        for (int i = 0; i < n; i++) {
            int tx = x + i * tw;
            bool act = (i == active);
            // Aktif: permukaan + underline aksen + teks primer.
            // Hover: permukaan. Inaktif: latar + teks sekunder.
            // Disabled: teks redup (tak bisa fokus/aktivasi).
            color_t tbg = act ? p.theme.surface
                        : (i == hover_idx) ? p.theme.surface
                                          : p.theme.bg;
            p.rect(tx, y, tw, STRIP_H, tbg);
            if (act) p.rect(tx, y + STRIP_H - 2, tw, 2, p.theme.accent);
            int tl = _ui_strlen(titles[i]) * 8;
            color_t tc = tab_disabled[i] ? p.theme.text_disabled
                       : act             ? p.theme.text
                                         : p.theme.text_secondary;
            p.text(titles[i], tx + (tw - tl) / 2, y + (STRIP_H - 16) / 2, tc);
        }
        // Focus ring strip (di dalam bounds) saat strip memegang fokus.
        if (has_focus && enabled) {
            p.rect(x, y, w, 1, p.theme.focus);
            p.rect(x, y + STRIP_H - 1, w, 1, p.theme.focus);
            p.rect(x, y, 1, STRIP_H, p.theme.focus);
            p.rect(x + w - 1, y, 1, STRIP_H, p.theme.focus);
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
