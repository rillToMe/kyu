// libs/widget/include/containers/tab.hpp — strip tab + panel aktif.
//
// BAHASA VISUAL TAB
// Tab horizontal di KyuzenOS TIDAK memakai "kotak yang menempel ke konten"
// (gaya browser lama). Yang dipakai:
//   * strip sebagai SATU bidang dengan garis dasar 1px (border_subtle),
//   * tab aktif ditandai UNDERLINE 2px beraksen + teks primer,
//   * tab tidak aktif = teks sekunder, tanpa latar,
//   * hover = latar surface_hover tipis.
//
// Hasilnya: strip tetap tenang walau ada banyak tab, dan yang aktif terbaca
// dari dua isyarat (teks + underline) tanpa perlu kotak.
#ifndef KWIDGET_CONTAINERS_TAB_HPP
#define KWIDGET_CONTAINERS_TAB_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/state.hpp"

namespace ui {

class Tab : public Widget {
public:
    enum { MAX_TABS = 8, STRIP_H = chrome::TAB_STRIP_H };
    char* titles[MAX_TABS];
    Widget* panels[MAX_TABS];
    bool tab_disabled[MAX_TABS];
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
    void set_tab_enabled(int i, int onv) {
        if (i < 0 || i >= n) return;
        tab_disabled[i] = (onv == 0);
        mark_dirty();
    }
    virtual void set_owner(Window* o) override {
        Widget::set_owner(o);
        for (int i = 0; i < n; i++)
            if (panels[i]) panels[i]->set_owner(o);
    }
    virtual int dirty_child_count() override { return n; }
    virtual Widget* dirty_child(int i) override { return panels[i]; }
    // Lebar tab mengikuti JUDUL (padding token), bukan dibagi rata: tab
    // "Umum" tidak boleh selebar tab "Personalization".
    int title_w(int i) const {
        return text_measure(titles[i]) + 2 * space::LG;
    }
    int title_x(int i) const {
        int tx = x;
        for (int j = 0; j < i; j++) tx += title_w(j);
        return tx;
    }
    // Tab pada posisi x (dibatasi lebar widget supaya tidak "bocor" ke luar).
    int title_at(int mx) const {
        if (n == 0 || mx < x || mx >= x + w) return -1;
        for (int i = 0; i < n; i++) {
            int tx = title_x(i);
            if (mx >= tx && mx < tx + title_w(i)) return i;
        }
        return -1;
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
            if (i >= 0 && i != active && !tab_disabled[i]) {
                mark_area(x, y, w, STRIP_H);   // strip lama + baru
                active = i;
                mark_dirty();
            }
            return;
        }
        if (active < n && panels[active]) {
            Widget* c = panels[active]->pick(mx, my);
            if (c) c->on_click(mx, my);
        }
    }
    // Strip bisa memegang fokus; panah pindah antar judul yang enabled.
    virtual bool focusable() override { return enabled; }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        (void)ascii; (void)mods;
        if (!enabled || n <= 0) return;
        uint32_t sc = scancode & 0xFF;
        int dir = 0;
        if (sc == 0x4B) dir = -1;        // Left
        else if (sc == 0x4D) dir = 1;    // Right
        else return;
        int start = active;
        for (int k = 0; k < n; k++) {
            active = (active + dir + n) % n;
            if (!tab_disabled[active]) { mark_dirty(); return; }
        }
        active = start;                   // semua disabled: tak berubah
    }
    virtual void set_hover(bool onv) override {
        if (!onv && hover_idx >= 0) { mark_title(hover_idx); hover_idx = -1; }
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
        mark_area(title_x(i), y, title_w(i), STRIP_H);
    }
    virtual void draw(Painter& p) override {
        const TypeRole& role = p.theme.type.label;
        // Latar strip = latar halaman (tab adalah bagian halaman, bukan panel).
        p.rect(x, y, w, STRIP_H, p.theme.bg);
        for (int i = 0; i < n; i++) {
            int tx = title_x(i);
            int tw = title_w(i);
            bool act = (i == active);
            bool hov = (i == hover_idx);
            if (hov && !act) p.surface_rect(tx, y, tw, STRIP_H, p.theme.surface_hover);
            int tl = text_measure(titles[i]);
            color_t tc = tab_disabled[i] ? p.theme.text_disabled
                       : act             ? p.theme.text
                                         : p.theme.text_secondary;
            p.text_role(titles[i], tx + (tw - tl) / 2,
                        y + text_vcenter(STRIP_H), tc, role);
            if (act) {
                // Underline aksen = penanda aktif. Digambar di dalam bounds.
                p.rect(tx + space::SM, y + STRIP_H - 2, tw - 2 * space::SM, 2,
                       p.theme.accent);
            }
        }
        // Garis dasar strip: satu garis, memisahkan strip dari panel.
        p.rect(x, y + STRIP_H - 1, w, 1, p.theme.border_subtle);
        // Fokus keyboard: outline tipis pada judul aktif (bukan seluruh strip).
        if (has_focus && enabled && active < n) {
            int tx = title_x(active), tw = title_w(active);
            p.rect(tx, y, tw, 1, p.theme.focus);
            p.rect(tx, y, 1, STRIP_H - 1, p.theme.focus);
            p.rect(tx + tw - 1, y, 1, STRIP_H - 1, p.theme.focus);
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
