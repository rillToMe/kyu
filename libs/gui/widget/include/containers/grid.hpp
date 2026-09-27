// libs/widget/include/containers/grid.hpp — Grid layout (Phase D).
#ifndef KWIDGET_CONTAINERS_GRID_HPP
#define KWIDGET_CONTAINERS_GRID_HPP

#include "layout/layout.hpp"

namespace ui {

// ------------------------------------------------------------
// Grid — kontainer baris × kolom (subclass Layout: ownership, dirty,
// pick, draw, traversal, dan invalidasi dipakai ulang utuh).
//
// Model ukuran (terdokumentasi, tanpa solver):
//   - anak: ukuran sendiri (fixed/preferred, konvensi toolkit)
//   - track: AUTO (isi) / FIXED (px) / FILL (sisa dibagi rata)
//   - padding: offset origin + menyusutkan area FILL
//   - gap: antar track (tanpa gap tepi; konsisten dengan VBox spacing)
//   - align: posisi anak di dalam sel (start/center/end/stretch)
// Algoritma: single-span dulu, spanning kedua (tanpa iterasi).
// ------------------------------------------------------------
class Grid : public Layout {
public:
    enum { MAX_RC = 8, MAX_CELLS = 32 };
    struct Cell { Widget* w; int r, c, rs, cs; };

    int rows, cols, gap;
    int pad_l, pad_t, pad_r, pad_b;
    int align;                    // UI_ALIGN_* (dalam sel)
    int col_mode[MAX_RC];         // UI_TRACK_* per kolom
    int col_px[MAX_RC];           // px bila FIXED
    int row_mode[MAX_RC];
    int row_px[MAX_RC];
    Cell cells[MAX_CELLS];
    int ncell;
    bool occ[MAX_RC][MAX_RC];     // hunian sel (overlap ditolak)

    Grid(int r, int c, int gap_px)
        : rows(r < 1 ? 1 : (r > MAX_RC ? MAX_RC : r)),
          cols(c < 1 ? 1 : (c > MAX_RC ? MAX_RC : c)),
          gap(gap_px < 0 ? 0 : gap_px),
          pad_l(0), pad_t(0), pad_r(0), pad_b(0),
          align(UI_ALIGN_STRETCH), ncell(0) {
        for (int i = 0; i < MAX_RC; i++) {
            col_mode[i] = UI_TRACK_AUTO; col_px[i] = 0;
            row_mode[i] = UI_TRACK_AUTO; row_px[i] = 0;
            for (int j = 0; j < MAX_RC; j++) occ[i][j] = false;
        }
        for (int i = 0; i < MAX_CELLS; i++) {
            cells[i].w = 0; cells[i].r = 0; cells[i].c = 0;
            cells[i].rs = 1; cells[i].cs = 1;
        }
    }
    void set_padding(int l, int t, int r, int b) {
        pad_l = l < 0 ? 0 : l; pad_t = t < 0 ? 0 : t;
        pad_r = r < 0 ? 0 : r; pad_b = b < 0 ? 0 : b;
        mark_dirty();
    }
    void set_align(int a) {
        align = (a >= UI_ALIGN_START && a <= UI_ALIGN_STRETCH) ? a : UI_ALIGN_STRETCH;
        mark_dirty();
    }
    void set_col(int c, int mode, int px) {
        if (c < 0 || c >= cols) return;
        col_mode[c] = (mode >= UI_TRACK_AUTO && mode <= UI_TRACK_FILL)
                          ? mode : UI_TRACK_AUTO;
        col_px[c] = px < 0 ? 0 : px;
        mark_dirty();
    }
    void set_row(int r, int mode, int px) {
        if (r < 0 || r >= rows) return;
        row_mode[r] = (mode >= UI_TRACK_AUTO && mode <= UI_TRACK_FILL)
                          ? mode : UI_TRACK_AUTO;
        row_px[r] = px < 0 ? 0 : px;
        mark_dirty();
    }
    // Taruh anak di sel (span dijepit muat; sel hunian/invalid → abaikan).
    // Return 1 terpasang, 0 diabaikan. Anak didaftarkan ke Layout::add
    // (ownership/dirty/traversal/fokus ikut mekanisme existing).
    int put(Widget* wgt, int r, int c, int rs, int cs) {
        if (!wgt || ncell >= MAX_CELLS) return 0;
        if (r < 0 || r >= rows || c < 0 || c >= cols) return 0;
        if (rs < 1) rs = 1;
        if (cs < 1) cs = 1;
        if (r + rs > rows) rs = rows - r;
        if (c + cs > cols) cs = cols - c;
        for (int i = r; i < r + rs; i++)
            for (int j = c; j < c + cs; j++)
                if (occ[i][j]) return 0;   // overlap → tolak deterministik
        for (int i = r; i < r + rs; i++)
            for (int j = c; j < c + cs; j++) occ[i][j] = true;
        cells[ncell].w = wgt;
        cells[ncell].r = r; cells[ncell].c = c;
        cells[ncell].rs = rs; cells[ncell].cs = cs;
        ncell++;
        add(wgt);   // Layout::add → owner + damage via mark di bawah
        mark_dirty();
        return 1;
    }
    // Ukuran track: isi + gap + padding (murni, untuk test & arrange).
    void measure_tracks(int cw[MAX_RC], int ch[MAX_RC]) {
        for (int i = 0; i < cols; i++) cw[i] = 0;
        for (int i = 0; i < rows; i++) ch[i] = 0;
        // FIXED + single-span AUTO.
        for (int k = 0; k < ncell; k++) {
            Cell& cl = cells[k];
            if (!cl.w || !cl.w->visible) continue;
            if (cl.cs == 1 && cl.c < cols && col_mode[cl.c] == UI_TRACK_AUTO &&
                cl.w->w > cw[cl.c])
                cw[cl.c] = cl.w->w;
            if (cl.rs == 1 && cl.r < rows && row_mode[cl.r] == UI_TRACK_AUTO &&
                cl.w->h > ch[cl.r])
                ch[cl.r] = cl.w->h;
        }
        for (int i = 0; i < cols; i++)
            if (col_mode[i] == UI_TRACK_FIXED) cw[i] = col_px[i];
        for (int i = 0; i < rows; i++)
            if (row_mode[i] == UI_TRACK_FIXED) ch[i] = row_px[i];
        // Spanning: pastikan total span muat (kembangkan track terakhir).
        for (int k = 0; k < ncell; k++) {
            Cell& cl = cells[k];
            if (!cl.w || !cl.w->visible) continue;
            if (cl.cs > 1) {
                int tot = 0;
                for (int j = cl.c; j < cl.c + cl.cs; j++) tot += cw[j];
                tot += gap * (cl.cs - 1);
                if (tot < cl.w->w) cw[cl.c + cl.cs - 1] += cl.w->w - tot;
            }
            if (cl.rs > 1) {
                int tot = 0;
                for (int i = cl.r; i < cl.r + cl.rs; i++) tot += ch[i];
                tot += gap * (cl.rs - 1);
                if (tot < cl.w->h) ch[cl.r + cl.rs - 1] += cl.w->h - tot;
            }
        }
        // FILL: sisa area (kontainer − padding − fixed/auto − gap) dibagi rata.
        int avail_w = w - pad_l - pad_r;
        int avail_h = h - pad_t - pad_b;
        int used_w = 0, fill_n = 0;
        for (int i = 0; i < cols; i++) {
            if (col_mode[i] == UI_TRACK_FILL) fill_n++;
            else used_w += cw[i];
        }
        used_w += gap * (cols - 1);
        if (fill_n > 0) {
            int rest = avail_w - used_w;
            if (rest < 0) rest = 0;
            int each = rest / fill_n, rem = rest % fill_n;
            for (int i = 0; i < cols; i++)
                if (col_mode[i] == UI_TRACK_FILL) {
                    cw[i] = each + (rem > 0 ? 1 : 0);
                    if (rem > 0) rem--;
                }
        }
        int used_h = 0, fill_m = 0;
        for (int i = 0; i < rows; i++) {
            if (row_mode[i] == UI_TRACK_FILL) fill_m++;
            else used_h += ch[i];
        }
        used_h += gap * (rows - 1);
        if (fill_m > 0) {
            int rest = avail_h - used_h;
            if (rest < 0) rest = 0;
            int each = rest / fill_m, rem = rest % fill_m;
            for (int i = 0; i < rows; i++)
                if (row_mode[i] == UI_TRACK_FILL) {
                    ch[i] = each + (rem > 0 ? 1 : 0);
                    if (rem > 0) rem--;
                }
        }
    }
    virtual void arrange() override {
        int cw[MAX_RC], ch[MAX_RC];
        measure_tracks(cw, ch);
        int cx[MAX_RC + 1], cy[MAX_RC + 1];
        cx[0] = x + pad_l;
        for (int i = 0; i < cols; i++) cx[i + 1] = cx[i] + cw[i] + gap;
        cy[0] = y + pad_t;
        for (int i = 0; i < rows; i++) cy[i + 1] = cy[i] + ch[i] + gap;
        for (int k = 0; k < ncell; k++) {
            Cell& cl = cells[k];
            if (!cl.w || !cl.w->visible) continue;
            int x0 = cx[cl.c], y0 = cy[cl.r];
            int span_w = cx[cl.c + cl.cs] - gap - x0;
            int span_h = cy[cl.r + cl.rs] - gap - y0;
            int nw = cl.w->w, nh = cl.w->h, nx = x0, ny = y0;
            if (align == UI_ALIGN_STRETCH) { nw = span_w; nh = span_h; }
            else {
                if (nw > span_w) nw = span_w;
                if (nh > span_h) nh = span_h;
                if (align == UI_ALIGN_CENTER) {
                    nx = x0 + (span_w - nw) / 2;
                    ny = y0 + (span_h - nh) / 2;
                } else if (align == UI_ALIGN_END) {
                    nx = x0 + span_w - nw;
                    ny = y0 + span_h - nh;
                }
            }
            cl.w->w = nw;
            cl.w->h = nh;
            place(cl.w, nx, ny);
        }
    }
    // Hanya sel terpasang yang digambar/di-hit (anak tanpa put() diabaikan).
    virtual void draw(Painter& p) override {
        arrange();
        for (int k = 0; k < ncell; k++)
            if (cells[k].w && cells[k].w->visible) cells[k].w->draw(p);
    }
    virtual Widget* pick(int mx, int my) override {
        if (!visible || !enabled) return 0;
        for (int k = ncell - 1; k >= 0; k--) {
            if (!cells[k].w) continue;
            Widget* r = cells[k].w->pick(mx, my);
            if (r) return r;
        }
        return 0;
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_GRID_HPP
