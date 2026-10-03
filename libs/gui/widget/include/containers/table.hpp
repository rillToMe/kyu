// libs/widget/include/containers/table.hpp — tabel kolom + baris scrollable.
//
// BAHASA VISUAL TABEL KYUZENOS
// Tabel adalah permukaan DATA: baris rapat, tanpa kotak per baris.
//   * Header = pita `surface_variant` (terbenam) dengan label peran `section`
//     (huruf besar, tone sekunder) + garis tipis di bawahnya. Header tidak
//     memakai warna aksen — ia label, bukan aksi.
//   * Baris memakai tinggi token `list::ROW_H` (24px) supaya sejajar dengan
//     ListView dan TreeView. Satu tinggi baris di seluruh aplikasi.
//   * Hover = `surface_hover`, terpilih = tinta `selection`. Bukan aksen penuh:
//     aksen disediakan untuk aksi/nilai, bukan untuk menandai "baris ini".
//   * Teks sel dipotong dengan ellipsis (teks panjang tidak bocor ke kolom
//     sebelahnya) dan digambar vertikal-center.
#ifndef KWIDGET_CONTAINERS_TABLE_HPP
#define KWIDGET_CONTAINERS_TABLE_HPP

#include "containers/scrollable.hpp"

namespace ui {

class Table : public Scrollable {
public:
    // Tinggi header = token daftar. Enum dipertahankan (nama lama) supaya
    // kode/test yang menyebut Table::HEADER_H tetap benar, tapi NILAINYA
    // sekarang dari token — bukan angka 24 yang ditulis terpisah.
    enum { HEADER_H = list::HEADER_H, MAX_COLS = 8, MAX_ROWS = 64 };
    char* col[MAX_COLS];
    int col_w[MAX_COLS];
    int ncols;
    char* cells[MAX_ROWS][MAX_COLS];
    int nrows;
    int selected, hover_row;
    ui_click_cb change_cb;
    void* change_data;
    // Ikon kecil opsional di kiri kolom pertama (non-owning, ARGB8888).
    // File Manager memakai ikon folder/tipe berkas; baris tanpa ikon digambar
    // seperti sebelumnya (tidak ada indentasi).
    enum { ICON_PX = 16, ICON_PAD = 4 };
    const uint32_t* icon[MAX_ROWS];
    int icon_w[MAX_ROWS], icon_h[MAX_ROWS];
    // Teks keadaan kosong (nrows == 0) — pasangan GridView::empty_text supaya
    // aplikasi tidak perlu widget terpisah hanya untuk "folder ini kosong".
    char empty_text[64];

    Table(int width, int height) : ncols(0), nrows(0), selected(-1),
                                   hover_row(-1), change_cb(0), change_data(0) {
        w = width; h = height;
        empty_text[0] = '\0';
        for (int c = 0; c < MAX_COLS; c++) col[c] = 0;
        for (int r = 0; r < MAX_ROWS; r++) {
            for (int c = 0; c < MAX_COLS; c++) cells[r][c] = 0;
            icon[r] = 0; icon_w[r] = icon_h[r] = 0;
        }
        set_scroll_view(0, h - header_h() - BAR_W);
    }
    virtual ~Table() {
        for (int c = 0; c < ncols; c++) _ui_free(col[c]);
        for (int r = 0; r < nrows; r++)
            for (int c = 0; c < ncols; c++) _ui_free(cells[r][c]);
    }
    // Tinggi header dari token daftar (satu tempat; konstruktor, scroll, dan
    // hit-test memakai ini supaya tidak ada tiga angka berbeda).
    int header_h() const { return list::HEADER_H; }
    void add_column(const char* title, int width) {
        if (ncols >= MAX_COLS) return;
        col[ncols] = _ui_strdup(title);
        col_w[ncols] = width;
        ncols++;
        mark_dirty();
    }
    void add_row(const char* const* vals, int n) {
        if (nrows >= MAX_ROWS || n > MAX_COLS) return;
        for (int c = 0; c < n; c++) cells[nrows][c] = _ui_strdup(vals[c]);
        for (int c = n; c < ncols; c++) cells[nrows][c] = 0;
        nrows++;
        set_scroll_view(nrows * ROW_H, h - header_h() - BAR_W);
        mark_dirty();
    }
    void clear() {
        for (int r = 0; r < nrows; r++)
            for (int c = 0; c < ncols; c++) { _ui_free(cells[r][c]); cells[r][c] = 0; }
        nrows = 0;
        // Baris lama sudah dibebaskan, jadi index seleksi/hover jadi stale:
        // baris hasil refresh berikutnya bisa ter-highlight "selected"/"hover"
        // padahal user tak pernah memilihnya. -1 = "tidak ada" (konvensi
        // constructor). change_cb SENGAJA tidak dipanggil — clear() bukan aksi
        // user, memanggil callback di sini adalah perilaku baru.
        selected = -1;
        hover_row = -1;
        for (int r = 0; r < MAX_ROWS; r++) { icon[r] = 0; icon_w[r] = icon_h[r] = 0; }
        set_scroll_view(0, h - header_h() - BAR_W);
        mark_dirty();
    }
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }
    void set_empty_text(const char* t) {
        int i = 0;
        if (t) for (; t[i] && i < (int)sizeof(empty_text) - 1; i++) empty_text[i] = t[i];
        empty_text[i] = '\0';
        mark_dirty();
    }
    // Ikon baris (non-owning). row di luar rentang → tanpa efek.
    void set_row_icon(int row, const uint32_t* px, int w_, int h_) {
        if (row < 0 || row >= MAX_ROWS) return;
        icon[row] = px; icon_w[row] = w_; icon_h[row] = h_;
        mark_dirty();
    }
    // Baris di bawah `my` (window-local) atau -1 (header / area kosong / luar).
    int row_at(int my) const {
        if (my < y + header_h() || my >= y + h) return -1;
        int r = (scroll + my - (y + header_h())) / ROW_H;
        return (r >= 0 && r < nrows) ? r : -1;
    }
    // Pilih baris dari kode + gulirkan masuk view. Tidak memanggil change_cb.
    void set_selected(int i) {
        if (i < -1 || i >= nrows || i == selected) return;
        selected = i;
        if (i >= 0) {
            int view_h = h - header_h() - BAR_W;
            int ry = i * ROW_H;
            if (ry < scroll) scroll = ry;
            else if (ry + ROW_H > scroll + view_h) scroll = ry + ROW_H - view_h;
            if (scroll < 0) scroll = 0;
            if (scroll > scroll_max) scroll = scroll_max;
        }
        mark_dirty();
    }
    virtual void set_hover(bool on) override { if (!on && hover_row >= 0) { hover_row = -1; mark_dirty(); } }
    virtual bool track_hover(int mx, int my) override {
        (void)mx;
        int r = row_at(my);
        if (r == hover_row) return false;
        hover_row = r;
        mark_dirty();
        return true;
    }
    virtual void on_content_click(int mx, int my) override {
        (void)mx;
        int r = row_at(my);
        if (r >= 0) {
            selected = r;
            mark_dirty();
            if (change_cb) change_cb(change_data);
        }
    }
    // Keyboard: panah pindah + memilih (native: tabel adalah satu stop fokus).
    virtual bool focusable() override { return enabled; }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        (void)ascii; (void)mods;
        if (!enabled || nrows <= 0) return;
        uint32_t sc = scancode & 0xFF;
        int nv = selected;
        switch (sc) {
        case 0x48: nv = selected - 1; break;              // Up
        case 0x50: nv = selected + 1; break;              // Down
        case 0x49: nv = selected - 5; break;              // PgUp
        case 0x51: nv = selected + 5; break;              // PgDn
        case 0x47: nv = 0; break;                         // Home
        case 0x4F: nv = nrows - 1; break;                 // End
        case 0x1C:                                        // Enter = aktivasi
            if (selected >= 0 && change_cb) change_cb(change_data);
            return;
        default: return;
        }
        if (nv < 0) nv = 0;
        if (nv > nrows - 1) nv = nrows - 1;
        if (nv == selected) return;
        set_selected(nv);
        if (change_cb) change_cb(change_data);
    }
    virtual void draw(Painter& p) override {
        const Metrics& m = p.theme.metrics;
        const TypeRole& hdr_role = p.theme.type.section;
        const TypeRole& cell_role = p.theme.type.body;
        const int hh = header_h();
        int cw = content_w();

        // Header: pita terbenam + label peran `section` + garis tipis.
        p.surface_rect(x, y, cw, hh, p.theme.surface_variant);
        int cx = x + m.sm;
        for (int c = 0; c < ncols; c++) {
            if (c == 0 && icon_column()) cx = x + m.sm;
            p.text_ellipsis(col[c], cx, y + text_vcenter(hh), col_w[c] - m.sm,
                            p.theme.tone(hdr_role.tone), hdr_role);
            cx += col_w[c];
        }
        p.rect(x, y + hh - 1, cw, 1, p.theme.border_subtle);

        if (nrows == 0 && empty_text[0]) {
            // Keadaan kosong: tone tersier, tidak berteriak.
            int ew = text_measure(empty_text);
            p.text_ellipsis(empty_text, x + m.sm,
                            y + hh + (h - hh) / 2 - glyph::HEIGHT / 2,
                            cw - 2 * m.sm, p.theme.text_tertiary, cell_role);
            (void)ew;
        }

        // Baris (scroll). Setiap sel dipotong ke kolomnya.
        p.set_clip(x, y + hh, cw, h - hh);
        for (int r = 0; r < nrows; r++) {
            int ry = y + hh + r * ROW_H - scroll;
            if (ry + ROW_H <= y + hh || ry >= y + h) continue;
            if (r == selected) p.surface_rect(x, ry, cw, ROW_H, p.theme.selection);
            else if (r == hover_row) p.surface_rect(x, ry, cw, ROW_H, p.theme.surface_hover);
            int cxx = x + m.sm;
            for (int c = 0; c < ncols; c++) {
                int avail = col_w[c] - m.sm;
                if (avail <= 0) { cxx += col_w[c]; continue; }
                p.set_clip(cxx, y + hh, avail, h - hh);
                int tx = cxx;
                if (c == 0 && icon[r]) {   // ikon + geser teks kolom pertama
                    p.image(cxx, ry + (ROW_H - ICON_PX) / 2, ICON_PX, ICON_PX,
                            icon[r], icon_w[r], icon_h[r]);
                    tx += ICON_PX + ICON_PAD;
                    avail -= ICON_PX + ICON_PAD;
                }
                if (cells[r][c] && avail > 0) {
                    p.text_ellipsis(cells[r][c], tx, ry + text_vcenter(ROW_H),
                                    avail, p.theme.text, cell_role);
                }
                p.set_clip(x, y + hh, cw, h - hh);
                cxx += col_w[c];
            }
        }
        p.clear_clip();
        draw_bar(p);
    }

private:
    // Kolom pertama menyediakan ruang ikon bila ADA baris yang berikon. Tanpa
    // ini, tabel tanpa ikon menyisakan indentasi kosong di semua baris.
    bool icon_column() const {
        for (int r = 0; r < nrows; r++) if (icon[r]) return true;
        return false;
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_TABLE_HPP
