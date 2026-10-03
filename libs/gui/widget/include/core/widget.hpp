// libs/widget/include/core/widget.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CORE_WIDGET_HPP
#define KWIDGET_CORE_WIDGET_HPP

#include "runtime/memory.hpp"

namespace ui {

class Window;    // fwd: Widget::owner — definisi lengkap di window/window.hpp
class Painter;   // fwd: Widget::draw(Painter&) — definisi di core/painter.hpp

// ------------------------------------------------------------
// Widget — basis pohon. Koordinat window-local konten.
// ------------------------------------------------------------
class Widget {
public:
    int x, y, w, h;
    bool visible;
    bool enabled;          // Phase B: false = redup + tak menerima input.
    bool has_focus;          // diset Window saat fokus keyboard intra-window
    ui_click_cb click_cb;
    void* userdata;
    // Menu konteks: klik kanan diteruskan ke widget di bawah kursor dengan
    // koordinat window-local (pemanggil tahu baris/sel mana yang diklik).
    ui_pos_click_cb right_cb;
    void* right_data;
    // Phase 9: Drag & Drop + bentuk kursor per-widget.
    bool draggable;
    char* dnd_payload;
    // Opt-out fokus keyboard. Sebagian widget bisa DIKLIK tapi tidak boleh
    // menjadi stop traversal Tab — mis. sidebar navigasi File Manager, yang
    // panah atas/bawahnya sudah dipakai daftar isi dan aplikasi yang menangani
    // F2/Delete. Tanpa flag ini, menjadikan sebuah kontainer fokusable akan
    // menelan tombol yang sebelumnya sampai ke aplikasi.
    bool focus_opt_out;
    // Phase D: tooltip presentasional per-widget (teks milik widget;
    // ditampilkan Window setelah hover delay, tanpa fokus/traversal).
    char* tooltip;
    bool drop_target;
    ui_drop_cb drop_cb;
    void* drop_data;
    int cursor_kind;
    // Phase 8: dirty tracking per-widget (tetap satu bbox di Window).
    // Widget yang benar-benar berubah menandai rect-nya sendiri; Window
    // meng-unions hanya rect itu, bukan seluruh pohon widget.
    bool dirty;
    int dm_x, dm_y, dm_w, dm_h;
    // Window pemilik widget (0 kalau belum dipasang). Dipakai operasi yang
    // mengubah TATA LETAK (mis. sembunyikan widget) supaya seluruh layar
    // digambar ulang, bukan hanya kotak widget itu sendiri.
    class Window* owner;

    Widget() : x(0), y(0), w(0), h(0), visible(true), enabled(true), has_focus(false),
               click_cb(0), userdata(0), right_cb(0), right_data(0),
               draggable(false), dnd_payload(0), tooltip(0),
              drop_target(false), drop_cb(0), drop_data(0),
              cursor_kind(UI_CURSOR_ARROW),
              dirty(false), dm_x(0), dm_y(0), dm_w(0), dm_h(0), owner(0),
              focus_opt_out(false) {}
    virtual ~Widget() { _ui_free(dnd_payload); _ui_free(tooltip); }
    // Pemilik dipasang Window saat widget masuk pohon (Layout menurunkan ke anak).
    virtual void set_owner(Window* o) { owner = o; }
    virtual void draw(Painter& p) = 0;
    virtual void set_hover(bool on) { (void)on; }
    virtual void set_focus(bool on) { has_focus = on; mark_dirty(); }
    // Tampil/sembunyi tanpa menghapus widget. Layout (VBox/HBox) melewati anak
    // yang tidak visible, jadi baris yang disembunyikan tidak makan tempat.
    // Definisi di luar class: butuh Window lengkap (damage_full).
    void set_visible(bool on);
    // Phase B: enabled/disabled generik. Disabled = digambar redup + pick()
    // mengembalikan 0 (tak bisa hover/klik/fokus). State transient (hover/
    // pressed) dibersihkan agar tak ada visual sticky.
    void set_enabled(bool on) {
        if (enabled == on) return;
        enabled = on;
        if (!on) { set_hover(false); on_release(); }
        mark_dirty();
    }
    virtual bool focusable() { return false; }   // TextBox → true
    // Opt-out fokus: dipakai Window::collect_focus() dan set_focus(). Ini satu
    // titik penegakan — widget yang menimpa focusable() tidak perlu tahu flag
    // ini, dan tidak bisa "lupa" menghormatinya.
    bool takes_focus() { return focus_opt_out ? false : focusable(); }
    void set_focus_opt_out(bool on) {
        focus_opt_out = on;
        if (on && has_focus) set_focus(false);
    }
    // Phase C: true = widget menelan Tab sendiri (editor multiline).
    // Traversal Tab melewati widget ini; false = Tab memindahkan fokus.
    virtual bool wants_tab() { return false; }
    // Phase 8: akumulasi rect kotor (window-local) — over-report BOLEH.
    void mark_area(int ax, int ay, int aw, int ah) {
        if (aw <= 0 || ah <= 0) return;
        if (!dirty) { dirty = true; dm_x = ax; dm_y = ay; dm_w = aw; dm_h = ah; return; }
        int x1 = ax + aw, y1 = ay + ah;
        int dx1 = dm_x + dm_w, dy1 = dm_y + dm_h;
        if (ax < dm_x) dm_x = ax;
        if (ay < dm_y) dm_y = ay;
        if (x1 > dx1) dx1 = x1;
        if (y1 > dy1) dy1 = y1;
        dm_w = dx1 - dm_x; dm_h = dy1 - dm_y;
    }
    void mark_dirty() { mark_area(x, y, w, h); }
    // Ambil + reset damage akumulatif widget.
    bool take_dirty(int& ox, int& oy, int& ow, int& oh) {
        if (!dirty) return false;
        ox = dm_x; oy = dm_y; ow = dm_w; oh = dm_h;
        dirty = false; dm_x = dm_y = dm_w = dm_h = 0;
        return true;
    }
    // Traversal dirty (Layout/Tab/ScrollView override).
    virtual int dirty_child_count() { return 0; }
    virtual Widget* dirty_child(int i) { (void)i; return 0; }
    // Layout: hitung ulang posisi anak sebelum pengumpulan damage (reflow).
    virtual void settle() {}
    // Hit-test: widget paling dalam yang memuat (mx,my), atau 0.
    // Disabled tidak bisa di-hit (tanpa hover/klik/fokus) — satu titik
    // penegakan untuk semua widget; override (Tab/Layout) mendelegasikan
    // ke anak yang menjaga dirinya sendiri.
    virtual Widget* pick(int mx, int my) {
        if (!visible || !enabled) return 0;
        return (mx >= x && mx < x + w && my >= y && my < y + h) ? this : 0;
    }
    // Phase 5: union bounds subtree ke (x0,y0,x1,y1) — untuk damage render luas
    // (mis. tick) tanpa region engine. Default = rect widget sendiri.
    virtual void collect_bounds(int& x0, int& y0, int& x1, int& y1) {
        if (!visible || w <= 0 || h <= 0) return;
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + w > x1) x1 = x + w;
        if (y + h > y1) y1 = y + h;
    }
    virtual void on_click(int mx, int my) {
        (void)mx; (void)my;
        if (click_cb) click_cb(userdata);
    }
    // Drag: dipanggil tiap MOUSE_MOVE selama mouse ditekan di widget ini
    // (grabbed). Return true = perlu redraw. on_release = tombol dilepas.
    virtual bool on_drag(int mx, int my) { (void)mx; (void)my; return false; }
    virtual void on_release() {}
    // Keyboard: hanya dipanggil bila widget ini yang punya fokus. ascii dari
    // P1 (0 = non-printable), scancode dari P3 (Backspace 0x0E, Enter 0x1C).
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) {
        (void)ascii; (void)scancode; (void)mods;
    }
    // Scroll roda (Phase 8): dipanggil saat EVENT_SCROLL lewat widget yang
    // sedang di-hover. delta = ±1 notch (+1 = roda ke bawah). Return true = redraw.
    virtual bool on_scroll(int delta) { (void)delta; return false; }
    // Hover sub-elemen (Phase 8): set_hover(bool) tidak membawa koordinat,
    // jadi widget ber-isi (Menu, MenuBar, Toolbar, ListView, Table, TreeView)
    // menimpa ini untuk melacak elemen mana yang di-hover. Return true = redraw.
    virtual bool track_hover(int mx, int my) { (void)mx; (void)my; return false; }
    // Bar menu (MenuBar) butuh perlakuan khusus di Window::run saat popup
    // terbuka (switch/close), beda dari bar lain (Toolbar) yang cukup close.
    virtual bool is_menu_bar() { return false; }
    void set_click(ui_click_cb cb, void* u) { click_cb = cb; userdata = u; }
    void set_right_click(ui_pos_click_cb cb, void* u) { right_cb = cb; right_data = u; }
    // Phase 9: DnD. Widget draggable memulai drag saat klik-tahan; click_cb
    // tidak dipanggil (threshold-drag untuk seret-langsung adalah masa depan).
    void set_draggable(const char* payload) {        char* n = _ui_strdup(payload ? payload : "");
        if (!n) return;
        _ui_free(dnd_payload);
        dnd_payload = n;
        draggable = true;
    }
    // Phase D: tooltip teks (disalin; 0/"" menghapus). Presentasional saja:
    // tanpa fokus, tanpa traversal, tanpa mengubah fokus aplikasi.
    void set_tooltip(const char* text) {
        char* n = (!text || !text[0]) ? 0 : _ui_strdup(text);
        if (text && text[0] && !n) return;
        _ui_free(tooltip);
        tooltip = n;
        mark_dirty();
    }
    void set_drop_target(ui_drop_cb cb, void* u) {
        drop_target = true; drop_cb = cb; drop_data = u;
    }
    // Phase D: dipanggil Window saat popup milik widget ini DITUTUP dari luar
    // (klik di luar, ESC, Tab, dtor) — sinkronkan flag internal. Default kosong.
    virtual void on_popup_dismiss() {}
    void set_cursor(int kind) { cursor_kind = kind; }
};

} // namespace ui

#endif // KWIDGET_CORE_WIDGET_HPP
