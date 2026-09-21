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

    Table(int width, int height) : ncols(0), nrows(0), selected(-1),
                                   hover_row(-1), change_cb(0), change_data(0) {
        w = width; h = height;
        for (int c = 0; c < MAX_COLS; c++) col[c] = 0;
        for (int r = 0; r < MAX_ROWS; r++)
            for (int c = 0; c < MAX_COLS; c++) cells[r][c] = 0;
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
        set_scroll_view(0, h - HEADER_H - BAR_W);
        mark_dirty();
    }
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }
    virtual void set_hover(bool on) override { if (!on && hover_row >= 0) { hover_row = -1; mark_dirty(); } }
    virtual bool track_hover(int mx, int my) override {
        (void)mx;
        int r = -1;
        if (my >= y + HEADER_H && my < y + h) {
            r = (scroll + my - (y + HEADER_H)) / ROW_H;
            if (r < 0 || r >= nrows) r = -1;
        }
        if (r == hover_row) return false;
        hover_row = r;
        mark_dirty();
        return true;
    }
    virtual void on_content_click(int mx, int my) override {
        (void)mx;
        int r = (scroll + my - (y + HEADER_H)) / ROW_H;
        if (r >= 0 && r < nrows) {
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
                if (cells[r][c]) p.text(cells[r][c], cxx, ry + 2, p.theme.fg);
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
