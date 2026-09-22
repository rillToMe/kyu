// libs/widget/include/containers/table.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CONTAINERS_TABLE_HPP
#define KWIDGET_CONTAINERS_TABLE_HPP

#include "containers/scrollable.hpp"

namespace ui {

// ------------------------------------------------------------
// Table — header tetap 24px + baris 20px yang bisa di-scroll.
// Setiap sel teks dipotong ke kolomnya (per-sel clip).
// ------------------------------------------------------------
class Table : public Scrollable {
public:
    enum { HEADER_H = 24, MAX_COLS = 8, MAX_ROWS = 64 };
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
        set_scroll_view(0, h - HEADER_H - BAR_W);
    }
    virtual ~Table() {
        for (int c = 0; c < ncols; c++) _ui_free(col[c]);
        for (int r = 0; r < nrows; r++)
            for (int c = 0; c < ncols; c++) _ui_free(cells[r][c]);
    }
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
        set_scroll_view(nrows * ROW_H, h - HEADER_H - BAR_W);
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
        set_scroll_view(0, h - HEADER_H - BAR_W);
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
        if (my < y + HEADER_H || my >= y + h) return -1;
        int r = (scroll + my - (y + HEADER_H)) / ROW_H;
        return (r >= 0 && r < nrows) ? r : -1;
    }
    // Pilih baris dari kode + gulirkan masuk view. Tidak memanggil change_cb.
    void set_selected(int i) {
        if (i < -1 || i >= nrows || i == selected) return;
        selected = i;
        if (i >= 0) {
            int view_h = h - HEADER_H - BAR_W;
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
    virtual void draw(Painter& p) override {
        int cw = content_w();
        // header tetap
        p.rect(x, y, cw, HEADER_H, p.theme.button_bg);
        int cx = x + 2;
        for (int c = 0; c < ncols; c++) {
            p.text(col[c], cx, y + 4, p.theme.button_fg);
            cx += col_w[c];
        }
        p.rect(x, y + HEADER_H - 1, cw, 1, p.theme.divider);   // pemisah halus, bukan garis fg terang
        if (nrows == 0 && empty_text[0]) {
            int ew = _ui_strlen(empty_text) * 8;
            p.text(empty_text, x + (cw - ew) / 2, y + HEADER_H + (h - HEADER_H) / 2 - 8,
                   p.theme.button_fg);
        }
        // baris (scroll), setiap sel dipotong ke kolomnya
        p.set_clip(x, y + HEADER_H, cw, h - HEADER_H);
        for (int r = 0; r < nrows; r++) {
            int ry = y + HEADER_H + r * ROW_H - scroll;
            if (ry + ROW_H <= y + HEADER_H || ry >= y + h) continue;
            if (r == selected) p.rect(x, ry, cw, ROW_H, p.theme.button_bg);
            else if (r == hover_row) p.rect(x, ry, cw, ROW_H, p.theme.button_hover);
            int cxx = x + 2;
            for (int c = 0; c < ncols; c++) {
                p.set_clip(cxx, y + HEADER_H, col_w[c] - 2, h - HEADER_H);
                int tx = cxx;
                if (c == 0 && icon[r]) {   // ikon + geser teks kolom pertama
                    p.image(cxx, ry + 2, ICON_PX, ICON_PX, icon[r], icon_w[r], icon_h[r]);
                    tx += ICON_PX + ICON_PAD;
                }
                if (cells[r][c]) p.text(cells[r][c], tx, ry + 2, p.theme.fg);
                p.set_clip(x, y + HEADER_H, cw, h - HEADER_H);
                cxx += col_w[c];
            }
        }
        p.clear_clip();
        draw_bar(p);
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_TABLE_HPP
