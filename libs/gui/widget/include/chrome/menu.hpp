// libs/widget/include/chrome/menu.hpp — Menu (popup) + MenuBar.
//
// BAHASA VISUAL MENU
//   * Permukaan popup = `panel` (satu level lebih terang dari halaman) +
//     border 1px + radius CONTAINER. Bayangan datang dari level elevasi
//     POPUP di Window::render — menu tidak menggambar bayangannya sendiri.
//   * Baris 24px, hover memakai surface_hover (bukan aksen penuh): menu
//     adalah daftar pilihan, bukan tombol.
//   * Kolom aksen kiri (gutter) hanya muncul saat ada item ber-centang,
//     supaya menu tanpa centang tidak punya ruang kosong menggantung.
//   * Accelerator ("Ctrl+S") memakai text_tertiary — lebih tenang dari label,
//     bukan warna kuning yang bersaing dengan isi menu.
//   * Ikon centang dari SISTEM IKON, bukan kotak aksen.
#ifndef KWIDGET_CHROME_MENU_HPP
#define KWIDGET_CHROME_MENU_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/theme.hpp"
#include "core/state.hpp"

namespace ui {

class Window;   // fwd: Menu pegang Window* — TIDAK include window.hpp

class Menu : public Widget {
public:
    // Item gaya menu desktop: label kiri, accelerator rata kanan, garis
    // pemisah, tanda centang, dan status enabled/disabled (redup).
    //
    // `id` = nama dari XML (opsional). Ini yang memungkinkan aplikasi menyetel
    // state item dengan NAMA ("view_list") alih-alih menjaga enum index baris
    // manual yang harus disinkronkan dengan urutan penambahan — sumber bug
    // yang nyata saat menu bertambah.
    struct Item {
        char* label;
        char* acc;          // teks shortcut rata kanan ("Ctrl+S"); 0 = tak ada
        char* id;           // id dari XML; 0 = tanpa id
        ui_click_cb cb;
        void* data;
        bool sep;           // 1 = garis pemisah (label/cb diabaikan)
        bool checked;
        bool disabled;
    };
    enum { MAX_ITEMS = 20 };
    Item items[MAX_ITEMS];
    int n, hover_idx;
    Window* win;

    Menu(Window* w) : n(0), hover_idx(-1), win(w) {
        this->w = 150;
        h = 4;
    }
    virtual ~Menu() {
        for (int i = 0; i < n; i++) {
            _ui_free(items[i].label);
            _ui_free(items[i].acc);
            _ui_free(items[i].id);
        }
    }
    // Index baris dengan id ini; -1 bila tidak ada. Dipakai set_checked_id()/
    // set_enabled_id() sehingga aplikasi tidak menyimpan index sendiri.
    int index_of_id(const char* id) const {
        if (!id || !id[0]) return -1;
        for (int i = 0; i < n; i++) {
            const char* a = items[i].id;
            if (!a) continue;
            int k = 0;
            while (a[k] && a[k] == id[k]) k++;
            if (a[k] == id[k]) return i;
        }
        return -1;
    }
    // Set state lewat id. Return 1 bila item ketemu.
    int set_checked_id(const char* id, int on) {
        int i = index_of_id(id);
        if (i < 0) return 0;
        set_checked(i, on);
        return 1;
    }
    int set_enabled_id(const char* id, int on) {
        int i = index_of_id(id);
        if (i < 0) return 0;
        set_enabled(i, on);
        return 1;
    }
    int row_h(int i) const {
        return items[i].sep ? chrome::MENU_SEP_H : chrome::MENU_ROW_H;
    }
    // Y baris ke-i — dijumlah dari baris sebelumnya, jadi baris pemisah
    // (tinggi berbeda) tidak merusak hit-test.
    int row_y(int i) const {
        int o = 2;
        for (int j = 0; j < i; j++) o += row_h(j);
        return y + o;
    }
    // Apakah menu ini punya item ber-centang? Menentukan kolom gutter.
    bool any_checked() const {
        for (int i = 0; i < n; i++)
            if (!items[i].sep && items[i].checked) return true;
        return false;
    }
    // Ukuran dari isi: baris terpanjang + kolom accelerator + gutter centang.
    void relayout() {
        const Metrics m;
        int hh = 2 * m.xs, need = 140;
        bool gutter = any_checked();
        for (int i = 0; i < n; i++) {
            hh += row_h(i);
            if (items[i].sep) continue;
            int t = text_measure(items[i].label) + 2 * m.md;
            if (gutter) t += m.icon_md + m.sm;
            if (items[i].acc) t += text_measure(items[i].acc) + m.lg;
            if (t > need) need = t;
        }
        // Menu bisa MENGCIL setelah relayout (w awal 150 → 130 untuk item
        // pendek). Tandai dulu bounds LAMA, baru bounds BARU: tanpa itu area
        // bekas menu yang lebih besar tak pernah diminta digambar ulang dan
        // sisa render lama (ghosting) tetap kelihatan.
        mark_area(x, y, w, h);
        h = hh;
        w = need;
        mark_area(x, y, w, h);
    }
    void add_item_acc(const char* label, const char* acc, ui_click_cb cb, void* u) {
        if (n >= MAX_ITEMS) return;
        items[n].label = _ui_strdup(label ? label : "");
        items[n].acc = acc ? _ui_strdup(acc) : 0;
        items[n].id = 0;
        items[n].cb = cb; items[n].data = u;
        items[n].sep = false; items[n].checked = false; items[n].disabled = false;
        n++;
        relayout();
        mark_dirty();
    }
    void add_item(const char* label, ui_click_cb cb, void* u) { add_item_acc(label, 0, cb, u); }
    // Item ber-id (dipakai inflater XML supaya binding memakai nama).
    // Return index item, atau -1 bila menu penuh — pemanggil TIDAK boleh
    // menebak index dari `n - 1` (saat penuh, n tidak bertambah dan index itu
    // menunjuk item SEBELUMNYA).
    int add_item_id(const char* id, const char* label, const char* acc,
                    ui_click_cb cb, void* u) {
        if (n >= MAX_ITEMS) return -1;
        add_item_acc(label, acc, cb, u);
        if (n == 0) return -1;                  // jaga-jaga
        int idx = n - 1;
        if (id && id[0]) {
            _ui_free(items[idx].id);
            items[idx].id = _ui_strdup(id);
        }
        return idx;
    }
    void add_sep() {
        if (n >= MAX_ITEMS) return;
        items[n].label = _ui_strdup(""); items[n].acc = 0; items[n].id = 0;
        items[n].cb = 0; items[n].data = 0;
        items[n].sep = true; items[n].checked = false; items[n].disabled = false;
        n++;
        relayout();
        mark_dirty();
    }
    void set_checked(int i, int onv) {
        if (i < 0 || i >= n) return;
        items[i].checked = (onv != 0);
        relayout();          // gutter bisa muncul/hilang
        mark_dirty();
    }
    void set_enabled(int i, int onv) {
        if (i < 0 || i >= n) return;
        items[i].disabled = (onv == 0);
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
    virtual void set_hover(bool onv) override {
        if (!onv && hover_idx >= 0) { mark_item(hover_idx); hover_idx = -1; }
    }
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
        const Metrics& m = p.theme.metrics;
        const TypeRole& role = p.theme.type.label;
        const int r = m.radius_container;
        // Permukaan popup + border 1px. Bayangan digambar Window (elevasi).
        p.surface(x, y, w, h, p.theme.panel, r);
        p.rrect_border(x, y, w, h, r, p.theme.border, 255);
        const bool gutter = any_checked();
        for (int i = 0; i < n; i++) {
            int ry = row_y(i);
            if (items[i].sep) {
                p.rect(x + m.md, ry + chrome::MENU_SEP_H / 2, w - 2 * m.md, 1,
                       p.theme.border_subtle);
                continue;
            }
            // Hover: inset 2px dari tepi supaya tidak menabrak border popup.
            if (i == hover_idx)
                p.rect(x + 2, ry, w - 4, chrome::MENU_ROW_H, p.theme.surface_hover);
            int tx = x + m.md;
            if (gutter) {
                if (items[i].checked) {
                    color_t ck = items[i].disabled ? p.theme.text_disabled
                                                   : p.theme.accent;
                    p.icon(ICON_CHECK, x + m.md + m.icon_md / 2, ry + chrome::MENU_ROW_H / 2,
                           m.icon_md, ck);
                }
                tx += m.icon_md + m.sm;
            }
            color_t fg = items[i].disabled ? p.theme.text_disabled : p.theme.text;
            int ty = ry + text_vcenter(chrome::MENU_ROW_H);
            if (items[i].acc) {
                int aw = text_measure(items[i].acc);
                // Accelerator dipotong agar tidak menabrak label.
                int lw = w - (tx - x) - aw - 2 * m.md;
                p.text_ellipsis(items[i].label, tx, ty, lw, fg, role);
                p.text(items[i].acc, x + w - aw - m.md, ty,
                       items[i].disabled ? p.theme.text_disabled : p.theme.text_tertiary);
            } else {
                p.text_ellipsis(items[i].label, tx, ty, w - (tx - x) - m.md, fg, role);
            }
        }
    }
    // out-of-class: butuh Window lengkap (close_popup)
    virtual void on_click(int mx, int my) override;
};

} // namespace ui

#endif // KWIDGET_CHROME_MENU_HPP
