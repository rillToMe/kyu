// libs/widget/include/containers/listview.hpp — daftar baris (primitif inti).
//
// DAFTAR = PRIMITIF UTAMA KYUZENOS
// Sebagian besar UI desktop sebenarnya adalah daftar: sidebar navigasi, baris
// pengaturan, hasil pencarian, daftar berkas. Karena itu baris daftar di sini
// punya bentuk yang jelas, bukan "teks di atas kotak seleksi":
//
//   [leading icon]  title                        [trailing]  [chevron]
//                   description
//
// Yang TIDAK dilakukan: membungkus tiap baris dalam kartu membulat. Baris
// dipisahkan oleh spasi dan (opsional) satu divider tipis — bukan border
// per baris. Itu yang membuat daftar panjang tetap tenang dibaca.
//
// STATE: normal / hover / selected / focused. Baris terpilih memakai tinta
// `selection` (bukan aksen penuh) supaya teks tetap jadi fokus utama; fokus
// keyboard ditandai outline DI DALAM baris.
#ifndef KWIDGET_CONTAINERS_LISTVIEW_HPP
#define KWIDGET_CONTAINERS_LISTVIEW_HPP

#include "containers/scrollable.hpp"

namespace ui {

// Satu baris daftar. Teks dimiliki ListView (disalin saat ditambah).
struct ListRow {
    char* title;
    char* description;      // 0 = baris satu-baris (tinggi normal)
    int icon;               // ICON_* (0 = tanpa ikon)
    int trailing;           // ICON_* di kanan sebelum chevron (0 = tidak ada)
    bool chevron;           // tampilkan chevron kanan (baris "menuju ke ...")
    bool disabled;
};

class ListView : public Scrollable {
public:
    enum { MAX_ITEMS = 32 };
    // Kompatibilitas: API lama memakai array `items` (label saja). Array ini
    // dipertahankan sebagai cermin `rows[i].title` supaya kode lama tetap
    // membaca label yang benar, dan supaya ABI ui_listview_* tidak berubah.
    char* items[MAX_ITEMS];
    ListRow rows[MAX_ITEMS];
    int n;
    int selected, hover_row;
    int focus_row;          // baris ber-fokus keyboard (-1 = tak ada)
    ui_click_cb change_cb;
    void* change_data;

    ListView(int width, int height) : n(0), selected(-1), hover_row(-1),
                                      focus_row(-1), change_cb(0), change_data(0) {
        w = width; h = height;
        for (int i = 0; i < MAX_ITEMS; i++) {
            items[i] = 0;
            rows[i].title = 0; rows[i].description = 0; rows[i].icon = ICON_NONE;
            rows[i].trailing = ICON_NONE; rows[i].chevron = false;
            rows[i].disabled = false;
        }
        set_scroll_max(0);
    }
    virtual ~ListView() {
        for (int i = 0; i < n; i++) {
            _ui_free(items[i]);
            _ui_free(rows[i].description);
        }
    }
    // Tinggi SATU baris. Dua-baris (ada description) memakai dua kali lipat
    // ditambah sedikit napas. Semua perhitungan scroll/hit-test memakai ini.
    int row_height(int i) const {
        return (rows[i].description && rows[i].description[0])
                   ? (ROW_H + 16) : ROW_H;
    }
    // Total tinggi konten — bukan n * ROW_H lagi (baris bisa beda tinggi).
    int content_height() const {
        int t = 0;
        for (int i = 0; i < n; i++) t += row_height(i);
        return t;
    }
    // Offset Y baris i relatif terhadap awal konten.
    int row_offset(int i) const {
        int o = 0;
        for (int k = 0; k < i; k++) o += row_height(k);
        return o;
    }
    int row_at(int my) const {
        if (my < y || my >= y + h) return -1;
        int rel = scroll + (my - y);
        int o = 0;
        for (int i = 0; i < n; i++) {
            int rh = row_height(i);
            if (rel >= o && rel < o + rh) return i;
            o += rh;
        }
        return -1;
    }
    void add_item(const char* label) {
        if (n >= MAX_ITEMS) return;
        items[n] = _ui_strdup(label ? label : "");
        rows[n].title = items[n];
        rows[n].description = 0;
        rows[n].icon = ICON_NONE;
        rows[n].trailing = ICON_NONE;
        rows[n].chevron = false;
        rows[n].disabled = false;
        n++;
        set_scroll_max(content_height());
        mark_dirty();
    }
    // Baris kaya: judul + deskripsi + ikon + chevron. Teks disalin.
    // Return index, atau -1 bila penuh.
    int add_row(const char* title, const char* description, int icon,
                bool chevron) {
        if (n >= MAX_ITEMS) return -1;
        int idx = n;
        add_item(title);
        if (n != idx + 1) return -1;      // penuh di tengah jalan
        if (description && description[0])
            rows[idx].description = _ui_strdup(description);
        rows[idx].icon = icon;
        rows[idx].chevron = chevron;
        set_scroll_max(content_height());
        mark_dirty();
        return idx;
    }
    void set_row_icon(int i, int icon) {
        if (i < 0 || i >= n) return;
        rows[i].icon = icon;
        mark_dirty();
    }
    void set_row_trailing(int i, int icon) {
        if (i < 0 || i >= n) return;
        rows[i].trailing = icon;
        mark_dirty();
    }
    void set_row_disabled(int i, bool d) {
        if (i < 0 || i >= n) return;
        rows[i].disabled = d;
        mark_dirty();
    }
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }
    // Pilih baris dari kode + gulirkan ke dalam view kalau perlu. Tidak
    // memanggil change_cb — pemanggilnya yang tahu dan menghindari rekursi.
    void set_selected(int i) {
        if (i < -1 || i >= n || i == selected) return;
        selected = i;
        if (i >= 0) ensure_visible(i);
        mark_dirty();
    }
    void ensure_visible(int i) {
        if (i < 0 || i >= n) return;
        int view_h = h - BAR_W;
        int ry = row_offset(i);
        int rh = row_height(i);
        if (ry < scroll) scroll = ry;
        else if (ry + rh > scroll + view_h) scroll = ry + rh - view_h;
        if (scroll < 0) scroll = 0;
        if (scroll > scroll_max) scroll = scroll_max;
    }
    virtual void set_hover(bool onv) override {
        if (!onv && hover_row >= 0) { hover_row = -1; mark_dirty(); }
    }
    virtual bool track_hover(int mx, int my) override {
        (void)mx;
        int r = row_at(my);
        if (r >= 0 && rows[r].disabled) r = -1;
        if (r == hover_row) return false;
        hover_row = r;
        mark_dirty();
        return true;
    }
    virtual void on_content_click(int mx, int my) override {
        (void)mx;
        int r = row_at(my);
        if (r >= 0 && r < n && !rows[r].disabled) {
            selected = r;
            focus_row = r;
            mark_dirty();
            if (change_cb) change_cb(change_data);
        }
    }
    // Keyboard: panah atas/bawah pindah + memilih (native: daftar adalah satu
    // stop fokus, jadi panah harus bekerja setelah Tab masuk ke sini).
    virtual bool focusable() override { return enabled; }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        (void)ascii; (void)mods;
        if (!enabled || n <= 0) return;
        uint32_t sc = scancode & 0xFF;
        int cur = focus_row >= 0 ? focus_row : selected;
        int nv = cur;
        switch (sc) {
        case 0x48: nv = cur - 1; break;                 // Up
        case 0x50: nv = cur + 1; break;                 // Down
        case 0x49: nv = cur - 5; break;                 // PgUp
        case 0x51: nv = cur + 5; break;                 // PgDn
        case 0x47: nv = 0; break;                       // Home
        case 0x4F: nv = n - 1; break;                   // End
        case 0x1C:                                      // Enter = aktivasi
            if (cur >= 0 && cur < n && !rows[cur].disabled && change_cb)
                change_cb(change_data);
            return;
        default: return;
        }
        if (nv < 0) nv = 0;
        if (nv > n - 1) nv = n - 1;
        // Lewati baris disabled (tetap berhenti kalau semua disabled).
        int start = nv, dir = nv >= cur ? 1 : -1;
        while (rows[nv].disabled) {
            nv += dir;
            if (nv < 0 || nv >= n || nv == start) { nv = start; break; }
        }
        if (nv == cur) return;
        focus_row = nv;
        selected = nv;
        ensure_visible(nv);
        mark_dirty();
        if (change_cb) change_cb(change_data);
    }
    virtual void draw(Painter& p) override {
        const Metrics& m = p.theme.metrics;
        const TypeRole& t_role = p.theme.type.body;
        const TypeRole& d_role = p.theme.type.caption;
        int cw = content_w();
        p.set_clip(x, y, cw, h);
        int oy = 0;
        for (int i = 0; i < n; i++) {
            int rh = row_height(i);
            int ry = y + oy - scroll;
            oy += rh;
            if (ry + rh <= y || ry >= y + h) continue;
            const ListRow& r = rows[i];
            bool sel = (i == selected);
            bool hov = (i == hover_row);
            // Latar baris: terpilih > hover. Disabled tidak menandai apa pun.
            if (sel)      p.rect(x, ry, cw, rh, p.theme.selection);
            else if (hov) p.rect(x, ry, cw, rh, p.theme.surface_hover);
            // Fokus keyboard: garis di tepi kiri baris (tenang, tidak
            // menutupi teks, dan tidak menggeser apa pun).
            if (i == focus_row && has_focus && enabled)
                p.rect(x, ry, 2, rh, p.theme.focus);

            color_t tc = r.disabled ? p.theme.text_disabled : p.theme.text;
            color_t sc = r.disabled ? p.theme.text_disabled : p.theme.text_secondary;
            int tx = x + m.md;
            // Ikon depan: kolom tetap supaya SEMUA judul rata kiri.
            if (r.icon != ICON_NONE) {
                p.icon(r.icon, x + m.md + m.icon_md / 2, ry + rh / 2,
                       m.icon_md, r.disabled ? p.theme.text_disabled : sc);
                tx = x + m.md + m.icon_md + m.sm;
            }
            // Ruang kanan untuk trailing/chevron supaya teks tidak menabrak.
            int right = m.md;
            if (r.chevron) right += m.icon_sm + m.sm;
            if (r.trailing != ICON_NONE) right += m.icon_md + m.sm;
            int avail = cw - (tx - x) - right;
            if (avail < 0) avail = 0;

            if (r.description && r.description[0]) {
                int ty = ry + m.xs + 2;
                p.text_ellipsis(r.title ? r.title : "", tx, ty, avail, tc, t_role);
                p.text_ellipsis(r.description, tx,
                                ty + t_role.bitmap_line_h, avail, sc, d_role);
            } else {
                p.text_ellipsis(r.title ? r.title : "", tx,
                                ry + text_vcenter(rh), avail, tc, t_role);
            }
            int rx = x + cw - m.md;
            if (r.chevron) {
                rx -= m.icon_sm;
                p.icon(ICON_CHEVRON_RIGHT, rx + m.icon_sm / 2, ry + rh / 2,
                       m.icon_sm, sc);
                rx -= m.sm;
            }
            if (r.trailing != ICON_NONE) {
                rx -= m.icon_md;
                p.icon(r.trailing, rx + m.icon_md / 2, ry + rh / 2, m.icon_md, sc);
            }
        }
        p.clear_clip();
        draw_bar(p);
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_LISTVIEW_HPP
