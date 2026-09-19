// libs/widget/include/chrome/menu.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CHROME_MENU_HPP
#define KWIDGET_CHROME_MENU_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/theme.hpp"

namespace ui {

class Window;   // fwd: Menu pegang Window* — TIDAK include window.hpp

// ------------------------------------------------------------
// Menu (popup) + MenuBar (bar title full-width).
// Menu::on_click & MenuBar::on_click/draw butuh Window lengkap
// (popup handling) → didefinisikan setelah class Window.
// ------------------------------------------------------------

class Menu : public Widget {
public:
    // Item gaya menu Windows: label kiri, accelerator rata kanan, garis
    // pemisah, tanda centang, dan status enabled/disabled (redup).
    struct Item {
        char* label;
        char* acc;          // teks shortcut rata kanan ("Ctrl+S"); 0 = tak ada
        ui_click_cb cb;
        void* data;
        bool sep;           // 1 = garis pemisah (label/cb diabaikan)
        bool checked;
        bool disabled;
    };
    enum { MAX_ITEMS = 20, ROW_H = 22, SEP_H = 9 };
    Item items[MAX_ITEMS];
    int n, hover_idx;
    Window* win;

    Menu(Window* w) : n(0), hover_idx(-1), win(w) {
        this->w = 150; h = 4;
    }
    virtual ~Menu() {
        for (int i = 0; i < n; i++) { _ui_free(items[i].label); _ui_free(items[i].acc); }
    }
    int row_h(int i) const { return items[i].sep ? SEP_H : ROW_H; }
    // Y layar baris ke-i — dijumlah dari baris sebelumnya, jadi baris pemisah
    // (tinggi berbeda) tidak merusak hit-test seperti rumus i*20.
    int row_y(int i) const {
        int o = 2;
        for (int j = 0; j < i; j++) o += row_h(j);
        return y + o;
    }
    // Ukuran dari isi: baris terpanjang + kolom accelerator + gutter centang.
    void relayout() {
        int hh = 4, need = 130;
        for (int i = 0; i < n; i++) {
            hh += row_h(i);
            if (items[i].sep) continue;
            int t = _ui_strlen(items[i].label) * 8 + 40;
            if (items[i].acc) t += _ui_strlen(items[i].acc) * 8 + 24;
            if (t > need) need = t;
        }
        h = hh;
        w = need;
    }
    void add_item_acc(const char* label, const char* acc, ui_click_cb cb, void* u) {
        if (n >= MAX_ITEMS) return;
        items[n].label = _ui_strdup(label ? label : "");
        items[n].acc = acc ? _ui_strdup(acc) : 0;
        items[n].cb = cb; items[n].data = u;
        items[n].sep = false; items[n].checked = false; items[n].disabled = false;
        n++;
        relayout();
        mark_dirty();
    }
    void add_item(const char* label, ui_click_cb cb, void* u) { add_item_acc(label, 0, cb, u); }
    void add_sep() {
        if (n >= MAX_ITEMS) return;
        items[n].label = _ui_strdup(""); items[n].acc = 0;
        items[n].cb = 0; items[n].data = 0;
        items[n].sep = true; items[n].checked = false; items[n].disabled = false;
        n++;
        relayout();
        mark_dirty();
    }
    void set_checked(int i, int on) {
        if (i < 0 || i >= n) return;
        items[i].checked = (on != 0);
        mark_item(i);
    }
    void set_enabled(int i, int on) {
        if (i < 0 || i >= n) return;
        items[i].disabled = (on == 0);
        mark_item(i);
    }
    // Baris di (mx,my); -1 bila di luar, baris pemisah, atau disabled.
    int item_at(int mx, int my) const {
        if (mx < x || mx >= x + w) return -1;
        for (int i = 0; i < n; i++) {
            int ry = row_y(i);
            if (my >= ry && my < ry + row_h(i))
                return (items[i].sep || items[i].disabled) ? -1 : i;
        }
        return -1;
    }
    void mark_item(int i) {
        if (i < 0 || i >= n) return;
        mark_area(x, row_y(i), w, row_h(i));
    }
    virtual void set_hover(bool on) override { if (!on && hover_idx >= 0) { mark_item(hover_idx); hover_idx = -1; } }
    virtual bool track_hover(int mx, int my) override {
        int i = -1;
        for (int k = 0; k < n; k++) {
            int ry = row_y(k);
            if (my >= ry && my < ry + row_h(k)) {
                if (mx >= x && mx < x + w && !items[k].sep && !items[k].disabled) i = k;
                break;
            }
        }
        if (i == hover_idx) return false;
        mark_item(hover_idx);
        hover_idx = i;
        mark_item(hover_idx);
        return true;
    }
    virtual void draw(Painter& p) override {
        // Popup menu = permukaan "panel" (lebih terang dari editor, senada
        // modal) + border halus, bukan kotak putih kontras.
        p.rect(x, y, w, h, p.theme.panel);
        p.rect(x, y, w, 1, p.theme.mborder);
        p.rect(x, y + h - 1, w, 1, p.theme.mborder);
        p.rect(x, y, 1, h, p.theme.mborder);
        p.rect(x + w - 1, y, 1, h, p.theme.mborder);
        for (int i = 0; i < n; i++) {
            int ry = row_y(i);
            if (items[i].sep) {
                p.rect(x + 8, ry + SEP_H / 2, w - 16, 1, p.theme.divider);
                continue;
            }
            if (i == hover_idx) p.rect(x + 1, ry, w - 2, ROW_H, p.theme.button_hover);
            // Kolom centang (View > Word Wrap) — kotak accent, bukan glyph,
            // karena font bitmap toolkit hanya punya ASCII.
            if (items[i].checked) p.rect(x + 8, ry + (ROW_H - 8) / 2, 8, 8, p.theme.accent);
            color_t fg = items[i].disabled ? color_darken(p.theme.fg, SHADE_55) : p.theme.fg;
            p.text(items[i].label, x + 24, ry + (ROW_H - 16) / 2, fg);
            if (items[i].acc) {
                // Shortcut ("Ctrl+S") pakai warna aksen khusus agar menonjol
                // dari deskripsi fungsinya (gaya hint kuning VS Code).
                int aw = _ui_strlen(items[i].acc) * 8;
                p.text(items[i].acc, x + w - aw - 12, ry + (ROW_H - 16) / 2,
                       items[i].disabled ? fg : p.theme.acc_text);
            }
        }
    }
    // out-of-class: butuh Window lengkap (close_popup)
    virtual void on_click(int mx, int my) override;
};

} // namespace ui

#endif // KWIDGET_CHROME_MENU_HPP
