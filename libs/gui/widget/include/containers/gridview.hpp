// libs/widget/include/containers/gridview.hpp — GridView: kontainer kisi
// generik (label + thumbnail) untuk aplikasi media.
//
// Kenapa widget baru: toolkit belum punya kontainer kisi — ListView/Table/
// TreeView hanya teks, dan Image = SATU berkas penuh-resolusi. Gallery butuh
// "banyak gambar kecil dalam kisi" tanpa membuat satu widget per item.
//
// GridView sengaja BODOH: tidak tahu apa itu filesystem, PNG, imageview, atau
// cache. Pemanggil menyuplai nama + piksel. Satu widget untuk seluruh kisi.
//
// Kepemilikan:
//   - nama item    : DIMILIKI GridView (_ui_strdup, pola ListView)
//   - piksel thumb : TIDAK dimiliki (pointer non-owning). App memegang cache-nya
//                    dan wajib memanggil set_thumb() lagi setelah isinya
//                    berubah; GridView hanya membaca saat draw().
//
// Virtualisasi (ratusan/ribuan item tanpa widget per item):
//   draw() hanya menggambar sel yang terlihat; app memakai visible_range()
//   untuk tahu sel mana yang perlu disiapkan (Gallery memuat thumbnail bertahap
//   satu per frame — lihat apps/gallery/thumbs.cpp).
#ifndef KWIDGET_CONTAINERS_GRIDVIEW_HPP
#define KWIDGET_CONTAINERS_GRIDVIEW_HPP

#include "containers/scrollable.hpp"

namespace ui {

class GridView : public Scrollable {
public:
    enum { MAX_ITEMS = 128, NAME_MAX = 32 };
    // Status sel yang belum punya piksel thumbnail.
    enum { PH_EMPTY = 0, PH_LOADING = 1, PH_ERROR = 2 };
    // Pad default kisi.
    enum { CELL_W_DEFAULT = 148, CELL_H_DEFAULT = 138, THUMB_BOX_DEFAULT = 112,
           PAD = 4, LABEL_H = 16 };

    struct Cell {
        char* name;             // dimiliki GridView
        const uint32_t* px;     // non-owning; pw×ph piksel ARGB8888
        int pw, ph;
        uint8_t placeholder;    // PH_*
    };

    Cell cells[MAX_ITEMS];
    int n;
    int selected, hover_cell;
    int cell_w, cell_h, thumb_box;
    ui_click_cb change_cb;      // seleksi berubah (klik / keyboard)
    void* change_data;
    ui_click_cb activate_cb;    // Enter / klik-kedua pada sel terpilih
    void* activate_data;
    char empty_text[64];        // teks keadaan kosong (n == 0)
    // Emulasi klik-ganda tanpa timer kernel: klik pada sel yang SUDAH terpilih
    // dalam jendela waktu ini dihitung activate. Tidak ada double-click di ABI
    // event (EVENT_MOUSE_CLICK tidak membawa hitungan klik).
    uint64_t last_click_ms;
    int last_click_cell;

    GridView(int width, int height)
        : n(0), selected(-1), hover_cell(-1),
          cell_w(CELL_W_DEFAULT), cell_h(CELL_H_DEFAULT),
          thumb_box(THUMB_BOX_DEFAULT),
          change_cb(0), change_data(0), activate_cb(0), activate_data(0),
          last_click_ms(0), last_click_cell(-1) {
        w = width; h = height;
        empty_text[0] = '\0';
        for (int i = 0; i < MAX_ITEMS; i++) {
            cells[i].name = 0;
            cells[i].px = 0;
            cells[i].pw = cells[i].ph = 0;
            cells[i].placeholder = PH_EMPTY;
        }
        set_scroll_max(0);
    }
    virtual ~GridView() { clear(); }

    // --- Isi ---
    void clear() {
        for (int i = 0; i < MAX_ITEMS; i++) {
            _ui_free(cells[i].name);
            cells[i].name = 0;
            cells[i].px = 0;
            cells[i].pw = cells[i].ph = 0;
            cells[i].placeholder = PH_EMPTY;
        }
        n = 0;
        selected = -1;
        hover_cell = -1;
        scroll = 0;
        set_scroll_max(0);
        mark_dirty();
    }
    // Tambah item; return index (>= 0) atau -1 bila kapasitas penuh.
    int add_item(const char* name) {
        if (n >= MAX_ITEMS) return -1;
        cells[n].name = _ui_strdup(name ? name : "");
        if (!cells[n].name) return -1;
        cells[n].px = 0;
        cells[n].pw = cells[n].ph = 0;
        cells[n].placeholder = PH_EMPTY;
        int idx = n++;
        set_scroll_max(row_count() * cell_h);
        mark_dirty();
        return idx;
    }
    void set_thumb(int i, const uint32_t* px, int pw, int ph) {
        if (i < 0 || i >= n) return;
        cells[i].px = px;
        cells[i].pw = pw;
        cells[i].ph = ph;
        cells[i].placeholder = (px && pw > 0 && ph > 0) ? PH_EMPTY : PH_LOADING;
        mark_dirty();
    }
    void set_placeholder(int i, int state) {
        if (i < 0 || i >= n) return;
        cells[i].px = 0;
        cells[i].pw = cells[i].ph = 0;
        cells[i].placeholder = (uint8_t)state;
        mark_dirty();
    }
    void set_empty_text(const char* t) {
        int i = 0;
        if (t) for (; t[i] && i < (int)sizeof(empty_text) - 1; i++) empty_text[i] = t[i];
        empty_text[i] = '\0';
        mark_dirty();
    }

    // --- Geometri ---
    void set_cell_size(int cw, int ch) {
        if (cw > 0) cell_w = cw;
        if (ch > 0) cell_h = ch;
        rebuild();
        mark_dirty();
    }
    void set_thumb_box(int px) {
        if (px > 0) thumb_box = px;
        rebuild();
        mark_dirty();
    }
    // Kolom dihitung dari lebar widget SELALU dengan lebar scrollbar
    // disisihkan: kalau bergantung pada tampil-tidaknya bar, bar bisa
    // muncul/hilang berosilasi tiap frame (lebar → kolom → tinggi → bar → lebar).
    int cols_for(int width) const {
        int cw = width > BAR_W ? width - BAR_W : width;
        int c = cell_w > 0 ? cw / cell_w : 1;
        return c < 1 ? 1 : c;
    }
    int columns() const { return cols_for(w); }
    int row_count() const {
        int c = columns();
        return n > 0 ? (n + c - 1) / c : 0;
    }
    void rebuild() { set_scroll_max(row_count() * cell_h); }

    // Rect sel i (koordinat window-local, sudah memperhitungkan scroll).
    void cell_rect(int i, int& cx, int& cy) const {
        int c = columns();
        cx = x + (i % c) * cell_w;
        cy = y + (i / c) * cell_h - scroll;
    }
    // Index sel pada (mx,my), atau -1 (termasuk area scrollbar).
    int cell_at(int mx, int my) const {
        if (mx < x || mx >= x + content_w() || my < y || my >= y + h) return -1;
        int c = columns();
        int col = (mx - x) / (cell_w > 0 ? cell_w : 1);
        int row = (scroll + my - y) / (cell_h > 0 ? cell_h : 1);
        if (col < 0 || col >= c || row < 0) return -1;
        int i = row * c + col;
        return (i >= 0 && i < n) ? i : -1;
    }
    // Rentang index sel yang terlihat (inklusif) — dasar virtualisasi +
    // pemuatan thumbnail bertahap oleh app.
    void visible_range(int& first, int& last) const {
        if (n <= 0) { first = 0; last = -1; return; }
        int c = columns();
        int view_h = h - BAR_W;
        if (view_h <= 0) view_h = h;
        int r0 = scroll / (cell_h > 0 ? cell_h : 1);
        int r1 = (scroll + view_h) / (cell_h > 0 ? cell_h : 1);
        if (r0 < 0) r0 = 0;
        int rows = row_count();
        if (r1 > rows - 1) r1 = rows - 1;
        first = r0 * c;
        last = (r1 + 1) * c - 1;
        if (first < 0) first = 0;
        if (first > n - 1) first = n - 1;
        if (last > n - 1) last = n - 1;
    }
    void ensure_visible(int i) {
        if (i < 0 || i >= n) return;
        int c = columns();
        int row = i / c;
        int view_h = h - BAR_W;
        if (view_h <= 0) view_h = h;
        int ry = row * cell_h;
        if (ry < scroll) scroll = ry;
        else if (ry + cell_h > scroll + view_h) scroll = ry + cell_h - view_h;
        if (scroll < 0) scroll = 0;
        if (scroll > scroll_max) scroll = scroll_max;
    }

    // --- Seleksi ---
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }
    void set_activate(ui_click_cb cb, void* u) { activate_cb = cb; activate_data = u; }
    virtual bool focusable() override { return true; }
    // notify = panggil change_cb (klik/keyboard: ya; pemanggil kode: tidak,
    // supaya tidak rekursi — pola ListView::set_selected).
    void set_selected_cell(int i, bool notify) {
        // -1 = "tidak ada yang terpilih" (sama seperti Table::set_selected):
        // tanpa ini, membatalkan seleksi dari kode (klik kanan di latar oleh
        // File Manager) meninggalkan sorotan lama di icon view. ensure_visible()
        // sudah menolak index < 0, jadi tidak ada gulir yang terjadi.
        if (i < -1 || i >= n || i == selected) return;
        selected = i;
        ensure_visible(i);
        mark_dirty();
        if (notify && change_cb) change_cb(change_data);
    }
    void activate_selected() {
        if (selected >= 0 && selected < n && activate_cb) activate_cb(activate_data);
    }

    // --- Input ---
    // Wheel: satu notch = satu baris sel (bukan ROW_H 20px milik Scrollable —
    // pada kisi, 20px membuat menggulir terasa tak bergerak).
    virtual bool on_scroll(int delta) override {
        int old = scroll;
        scroll += delta * cell_h;
        if (scroll < 0) scroll = 0;
        if (scroll > scroll_max) scroll = scroll_max;
        if (scroll != old) { mark_dirty(); return true; }
        return false;
    }
    virtual bool track_hover(int mx, int my) override {
        int c = cell_at(mx, my);
        if (c == hover_cell) return false;
        hover_cell = c;
        mark_dirty();
        return true;
    }
    virtual void set_hover(bool on) override {
        if (!on && hover_cell >= 0) { hover_cell = -1; mark_dirty(); }
    }
    virtual void on_content_click(int mx, int my) override {
        int i = cell_at(mx, my);
        if (i < 0) return;
        uint64_t now = sys_uptime();
        // Klik kedua pada sel yang sama dalam 400 ms = activate (klik-ganda).
        bool dbl = (i == last_click_cell && i == selected &&
                    now - last_click_ms < 400);
        last_click_ms = now;
        last_click_cell = i;
        if (dbl) { activate_selected(); return; }
        set_selected_cell(i, true);
    }
    // Navigasi keyboard grid. Scancode set-1 dengan bit extended 0x100
    // (arrow/Home/End/PgUp/PgDn) — sama seperti yang diteruskan Window.
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        (void)mods;
        if (n <= 0) return;
        int c = columns();
        int rows_vis = (h - BAR_W) / (cell_h > 0 ? cell_h : 1);
        if (rows_vis < 1) rows_vis = 1;
        int cur = selected < 0 ? 0 : selected;
        int next = cur;
        switch (scancode) {
        case 0x14B: next = cur - 1; break;                  // Left
        case 0x14D: next = cur + 1; break;                  // Right
        case 0x148: next = cur - c; break;                  // Up
        case 0x150: next = cur + c; break;                  // Down
        case 0x147: next = 0; break;                        // Home
        case 0x14F: next = n - 1; break;                    // End
        case 0x149: next = cur - rows_vis * c; break;       // PageUp
        case 0x151: next = cur + rows_vis * c; break;       // PageDown
        case 0x1C:                                          // Enter
            activate_selected();
            return;
        default:
            if (ascii == '\n' || ascii == '\r') { activate_selected(); return; }
            return;
        }
        if (next < 0) next = 0;
        if (next > n - 1) next = n - 1;
        if (next == selected) return;
        set_selected_cell(next, true);
    }

    // --- Render ---
    // Nama dipotong agar muat satu sel; ".." sebagai elipsis (font8x16 hanya
    // mencakup ASCII, jadi bukan karakter '…').
    void label_for(const Cell& cell, char* out, int out_cap, int max_chars) const {
        int i = 0;
        if (cell.name) {
            for (; cell.name[i] && i < max_chars && i < out_cap - 3; i++)
                out[i] = cell.name[i];
        }
        if (cell.name && cell.name[i]) { out[i++] = '.'; out[i++] = '.'; }
        out[i] = '\0';
    }

    virtual void draw(Painter& p) override {
        rebuild();
        int cw = content_w();
        p.set_clip(x, y, cw, h);
        if (n == 0 && empty_text[0]) {
            int tw = _ui_strlen(empty_text) * 8;
            p.text(empty_text, x + (cw - tw) / 2, y + h / 2 - 8, p.theme.fg);
        }
        int first, last;
        visible_range(first, last);
        int inner_w = cell_w - 2 * PAD;
        int max_chars = inner_w / 8;
        char label[NAME_MAX + 3];
        for (int i = first; i <= last; i++) {
            int cx, cy;
            cell_rect(i, cx, cy);
            if (i == selected) p.rect(cx + 1, cy + 1, cell_w - 2, cell_h - 2, p.theme.button_bg);
            else if (i == hover_cell) p.rect(cx + 1, cy + 1, cell_w - 2, cell_h - 2, p.theme.button_hover);
            const Cell& cell = cells[i];
            int tx = cx + PAD;
            int ty = cy + PAD;
            if (cell.px && cell.pw > 0 && cell.ph > 0) {
                // Thumbnail sudah seukuran kotak (app yang men-skalasi sekali di
                // cache) → blit 1:1, bukan skala ulang tiap frame.
                p.image(tx + (inner_w - cell.pw) / 2,
                        ty + (thumb_box - cell.ph) / 2,
                        cell.pw, cell.ph, cell.px, cell.pw, cell.ph);
            } else {
                p.rect(tx, ty, inner_w, thumb_box, p.theme.button_bg);
                if (cell.placeholder == PH_ERROR) {
                    int mw = thumb_box / 2;
                    p.rect(tx + (inner_w - mw) / 2, ty + (thumb_box - mw) / 2, mw, mw,
                           p.theme.button_hover);
                    p.text("!", tx + inner_w / 2 - 4, ty + thumb_box / 2 - 8, p.theme.fg);
                }
            }
            label_for(cell, label, (int)sizeof(label), max_chars);
            int lw = _ui_strlen(label) * 8;
            p.text(label, cx + (cell_w - lw) / 2, cy + PAD + thumb_box + 2, p.theme.fg);
        }
        p.clear_clip();
        draw_bar(p);
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_GRIDVIEW_HPP
