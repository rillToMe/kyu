// libs/widget/include/primitives/combobox.hpp — ComboBox (Phase D).
#ifndef KWIDGET_PRIMITIVES_COMBOBOX_HPP
#define KWIDGET_PRIMITIVES_COMBOBOX_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "chrome/menu.hpp"

namespace ui {

class Window;   // fwd: ComboBox pegang Window* — TIDAK include window.hpp

// ------------------------------------------------------------
// ComboBox — kotak tertutup + popup Menu (reuse arsitektur popup
// existing: Window::open_popup/close_popup, bukan framework baru).
// Model data gaya toolkit: array tetap, string milik ComboBox (_ui_strdup,
// pola ListView). Popup + pick-trampolin dimiliki ComboBox.
// ------------------------------------------------------------
class ComboBox : public Widget {
public:
    enum { MAX_ITEMS = 16 };
    struct Pick { ComboBox* self; int idx; };   // userdata item menu popup

    char* items[MAX_ITEMS];
    int n;
    int selected;            // -1 = tak ada
    bool open;
    bool hover;
    Menu* menu;              // popup (dibuat ulang tiap dibuka)
    Pick picks[MAX_ITEMS];
    int n_picks;
    Window* win;
    ui_click_cb change_cb;
    void* change_data;

    ComboBox() : n(0), selected(-1), open(false), hover(false),
                 menu(0), n_picks(0), win(0), change_cb(0), change_data(0) {
        w = 160; h = 24;
        for (int i = 0; i < MAX_ITEMS; i++) items[i] = 0;
        cursor_kind = UI_CURSOR_HAND;
    }
    virtual ~ComboBox();
    void free_picks() { n_picks = 0; }   // Pick = nilai, bukan alokasi
    // Propagasi owner susulan (pola Tab/ScrollView).
    virtual void set_owner(Window* o) override {
        Widget::set_owner(o);
        win = o;
    }
    // --- Data (caller-owned di API; disalin ke milik sendiri) ---
    int add_item(const char* label) {
        if (n >= MAX_ITEMS) return -1;
        items[n] = _ui_strdup(label ? label : "");
        if (!items[n]) return -1;
        mark_dirty();
        return n++;
    }
    int remove_item(int i) {
        if (i < 0 || i >= n) return 0;
        _ui_free(items[i]);
        for (int j = i; j + 1 < n; j++) items[j] = items[j + 1];
        items[--n] = 0;
        if (selected == i) selected = -1;
        else if (selected > i) selected--;
        mark_dirty();
        return 1;
    }
    void clear() {
        for (int i = 0; i < n; i++) { _ui_free(items[i]); items[i] = 0; }
        n = 0;
        selected = -1;
        mark_dirty();
    }
    int count() const { return n; }
    // Programatik diam-diam (konvensi ListView/Table: tanpa change_cb).
    void set_selected(int i) {
        if (i < -1 || i >= n || i == selected) return;
        selected = i;
        mark_dirty();
    }
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }

    virtual bool focusable() override { return enabled; }
    virtual void set_hover(bool on) override { hover = on; mark_dirty(); }
    // Window memanggil ini saat popup DITUTUP dari luar (klik di luar, ESC,
    // Tab, atau dtor) — sinkronkan flag + bebaskan pick sekali pakai.
    virtual void on_popup_dismiss() override {
        open = false;
        free_picks();
        mark_dirty();
    }
    void dismiss();
    void open_menu();
    void pick_from_menu(int i) {
        set_selected(i);
        if (change_cb && i >= 0) change_cb(change_data);
        dismiss();
    }
    static void pick_trampoline(void* u) {
        Pick* k = (Pick*)u;
        if (k && k->self) k->self->pick_from_menu(k->idx);
    }
    virtual void on_click(int mx, int my) override {
        (void)mx; (void)my;
        if (!enabled) return;
        if (open) dismiss();
        else open_menu();
    }
    // Keyboard: tertutup + Enter/Spasi = buka; panah = navigasi langsung
    // (native); terbuka + panah = gerakkan hover; Enter = commit + tutup.
    // Esc ditangani Window (tutup popup); Tab menutup via hook Window.
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override;
    virtual void draw(Painter& p) override;
};

// open_menu() dideklarasikan di dalam class; definisinya (butuh Window
// lengkap) ada di src/primitives/combobox.cpp — tanpa inline agar satu
// definisi di seluruh TU (aturan ODR).

inline void ComboBox::on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) {
    (void)mods;
    if (!enabled) return;
    mark_dirty();
    uint32_t sc = scancode & 0xFF;
    if (sc == 0x48 || sc == 0x50) {           // Up / Down (set-1)
        if (!open) {
            // Tertutup: navigasi langsung (native), tanpa popup.
            int nv = selected + (sc == 0x50 ? 1 : -1);
            if (nv < 0) nv = 0;
            if (nv > n - 1) nv = n - 1;
            if (nv != selected && nv >= 0 && nv < n) {
                selected = nv;
                if (change_cb) change_cb(change_data);
            }
            return;
        }
        if (menu) {
            int h = menu->hover_idx;
            if (h < 0) h = selected;
            h += (sc == 0x50 ? 1 : -1);
            if (h < 0) h = 0;
            if (h > n - 1) h = n - 1;
            menu->mark_item(menu->hover_idx);
            menu->hover_idx = h;
            menu->mark_item(h);
        }
        return;
    }
    if (ascii == '\n' || ascii == '\r' || ascii == ' ' || sc == 0x1C) {
        if (!open) { open_menu(); return; }
        int i = (menu && menu->hover_idx >= 0) ? menu->hover_idx : selected;
        if (i >= 0 && i < n) pick_from_menu(i);
        else dismiss();
    }
}

inline void ComboBox::draw(Painter& p) {
    color_t txt = enabled ? p.theme.text : p.theme.text_disabled;
    p.rect(x, y, w, h, p.theme.surface);
    // Border 1px: subtle → hover → focus (terbuka = fokus logis).
    color_t border = !enabled        ? p.theme.border_subtle
                   : (has_focus || open) ? p.theme.focus
                   : hover           ? p.theme.border
                                     : p.theme.border_subtle;
    p.rect(x, y, w, 1, border);
    p.rect(x, y + h - 1, w, 1, border);
    p.rect(x, y, 1, h, border);
    p.rect(x + w - 1, y, 1, h, border);
    // Label terpilih, dipotong sebelum panah (lebar panah 18px).
    p.set_clip(x + 4, y, w - 22, h);
    if (selected >= 0 && selected < n) p.text(items[selected], x + 4, y + 4, txt);
    p.clear_clip();
    // Panah bawah (segitiga piksel, gaya native).
    int ax = x + w - 13, ay = y + (h - 4) / 2;
    color_t ac = enabled ? p.theme.text_secondary : p.theme.text_disabled;
    p.rect(ax, ay, 7, 1, ac);
    p.rect(ax + 1, ay + 1, 5, 1, ac);
    p.rect(ax + 2, ay + 2, 3, 1, ac);
    p.rect(ax + 3, ay + 3, 1, 1, ac);
}

} // namespace ui

#endif // KWIDGET_PRIMITIVES_COMBOBOX_HPP
