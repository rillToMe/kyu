// libs/widget/include/dialog/dialog.hpp — modal overlay di tengah window.
//
// BAHASA VISUAL MODAL
// Dialog adalah satu-satunya permukaan yang boleh benar-benar "mengambang",
// jadi ia memakai SELURUH anggaran kedalaman dengan hemat:
//   * permukaan `panel` + radius DIALOG + border 1px + bayangan level DIALOG
//     (dari token elevasi, digambar Window::render — bukan oleh dialog).
//   * Hierarki dibangun oleh TIPOGRAFI, bukan oleh garis aksen tebal:
//     judul (role title) → pemisah halus → isi (role body) → baris tombol.
//     Garis aksen 2px di tepi atas DIHAPUS: ia dekorasi, bukan informasi.
//   * Tombol: aksi pertama = PRIMARY, sisanya SECONDARY. Itu hierarki yang
//     sama dengan tombol di halaman — modal tidak punya bahasa tombol sendiri.
//
// Non-blocking: cb(index) dipanggil saat tombol ditekan; index = -1 bila
// dibatalkan (ESC).
#ifndef KWIDGET_DIALOG_DIALOG_HPP
#define KWIDGET_DIALOG_DIALOG_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/state.hpp"
#include "runtime/memory.hpp"

namespace ui {

class Window;   // fwd: Dialog pegang Window* — TIDAK include window.hpp

// Baris isi yang DIAWALI prefix ini digambar dengan warna `accent_text` —
// aplikasi menandai baris shortcut ("Ctrl+N") agar menonjol dari deskripsi.
// Dipertahankan untuk kompatibilitas (dipakai app yang sudah ada).
#define UI_ACCENT_PREFIX "#> "
static const int UI_ACCENT_PREFIX_LEN = 3;

class Dialog : public Widget {
public:
    enum { MAX_BTNS = 4 };
    char* title;
    char* text;
    char* btns[MAX_BTNS];
    int n_btns;
    int hover_btn;
    ui_dialog_cb cb;
    void* data;
    Window* win;

    Dialog(Window* w, const char* t, const char* tx,
           const char* const* b, int n, ui_dialog_cb c, void* d)
        : title(_ui_strdup(t ? t : "")), text(_ui_strdup(tx ? tx : "")),
          n_btns(n < MAX_BTNS ? n : MAX_BTNS), hover_btn(-1),
          cb(c), data(d), win(w) {
        for (int i = 0; i < MAX_BTNS; i++) btns[i] = 0;
        for (int i = 0; i < n_btns; i++) btns[i] = _ui_strdup(b[i] ? b[i] : "");
        layout();
    }
    virtual ~Dialog() {
        _ui_free(title); _ui_free(text);
        for (int i = 0; i < MAX_BTNS; i++) _ui_free(btns[i]);
    }
    // Ukuran dari isi. Dihitung ulang lewat layout() supaya perubahan teks
    // (kalau nanti diperlukan) tidak butuh konstruktor baru.
    void layout() {
        const Metrics m;
        const Typography ty;
        int nlines = 1;
        for (int i = 0; text[i]; i++) if (text[i] == '\n') nlines++;
        int wid = m.xl + 2 * m.xl;            // minimum yang nyaman
        int cand = text_width(title, ty.title) + 2 * m.xl;
        if (cand > wid) wid = cand;
        // Baris isi terpanjang.
        int start = 0, i = 0;
        for (;;) {
            if (text[i] == '\n' || text[i] == '\0') {
                int len = text_width_sub(text, start, i, ty.body) + 2 * m.xl;
                if (len > wid) wid = len;
                if (text[i] == '\0') break;
                start = i + 1;
            }
            i++;
        }
        int bw = 0;
        for (int i2 = 0; i2 < n_btns; i2++) bw += btn_w(i2) + m.sm;
        if (bw - m.sm + 2 * m.xl > wid) wid = bw - m.sm + 2 * m.xl;
        if (wid < 300) wid = 300;
        this->w = wid;
        // judul + pemisah + isi + napas + tombol.
        this->h = m.md + ty.title.bitmap_line_h + m.md
                + nlines * ty.body.bitmap_line_h + m.lg
                + m.control_h + m.lg;
    }
    // Lebar teks [from,to) pada peran r (tanpa alokasi).
    static int text_width_sub(const char* s, int from, int to, const TypeRole& r) {
        int n = to - from;
        if (n < 0) n = 0;
        return n * (glyph::ADVANCE + r.bitmap_tracking);
    }
    int btn_row_y() const { return y + h - (Metrics().lg + Metrics().control_h); }
    int btn_total() const {
        const Metrics m;
        int t = 0;
        for (int i = 0; i < n_btns; i++) t += btn_w(i) + m.sm;
        return t > 0 ? t - m.sm : 0;
    }
    int btn_x(int i) const {
        const Metrics m;
        int bx = x + (w - btn_total()) / 2;
        for (int j = 0; j < i; j++) bx += btn_w(j) + m.sm;
        return bx;
    }
    int btn_w(int i) const {
        const Metrics m;
        return _ui_strlen(btns[i]) * glyph::ADVANCE + 2 * m.control_pad_x;
    }
    int hit_button(int mx, int my) const {
        const Metrics m;
        int by = btn_row_y();
        if (my < by || my >= by + m.control_h) return -1;
        for (int i = 0; i < n_btns; i++)
            if (mx >= btn_x(i) && mx < btn_x(i) + btn_w(i)) return i;
        return -1;
    }
    // Dipanggil Window saat ESC menutup dialog, SEBELUM objek ini dihapus.
    // PromptDialog memakainya untuk mengirim "dibatalkan" ke aplikasi.
    virtual void on_cancel() {}
    // Dialog = satu stop fokus modal. Kiri/Kanan pindah hover antar tombol,
    // Enter/Spasi aktifkan.
    virtual bool focusable() override { return true; }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override;
    virtual void draw(Painter& p) override {
        const Metrics& m = p.theme.metrics;
        const Typography& ty = p.theme.type;
        const int r = m.radius_dialog;
        // Permukaan modal. Bayangan digambar Window pada level DIALOG.
        p.surface(x, y, w, h, p.theme.panel, r);
        // Border: warna fokus saat dialog memegang fokus keyboard, kalau tidak
        // border biasa. Satu garis, bukan dua.
        color_t edge = has_focus ? p.theme.focus : p.theme.border;
        p.rrect_border(x, y, w, h, r, edge, 255);
        // Judul (role title) + pemisah halus di bawahnya.
        int ty0 = y + m.md;
        p.text(title, x + m.xl, ty0, p.theme.text);
        int div_y = ty0 + ty.title.bitmap_line_h - 2;
        p.rect(x + m.xl, div_y, w - 2 * m.xl, 1, p.theme.border_subtle);
        // Isi: baris ber-prefix aksen memakai accent_text (kompatibilitas).
        {
            int by = div_y + m.md;
            int start = 0, i = 0;
            for (;;) {
                if (text[i] == '\n' || text[i] == '\0') {
                    bool acc = (_ui_strncmp(text + start, UI_ACCENT_PREFIX,
                                            UI_ACCENT_PREFIX_LEN) == 0);
                    char save = text[i];
                    // p.text() menggambar sampai NUL — pinjam byte baris itu.
                    const_cast<char*>(text)[i] = '\0';
                    p.text(text + start, x + m.xl, by,
                           acc ? p.theme.accent_text : p.theme.text);
                    const_cast<char*>(text)[i] = save;
                    if (save == '\0') break;
                    start = i + 1;
                    by += ty.body.bitmap_line_h;
                }
                i++;
            }
        }
        // Baris tombol: primer (indeks 0) + sekunder. Radius kontrol yang sama
        // dengan Button di halaman.
        int byy = btn_row_y();
        for (int i = 0; i < n_btns; i++) {
            int bx = btn_x(i);
            bool primary = (i == 0);
            bool hov = (i == hover_btn);
            color_t fill, txt, bd;
            if (primary) {
                fill = hov ? p.theme.accent_hover : p.theme.accent;
                txt = p.theme.accent_contrast;
                bd = fill;
            } else {
                fill = hov ? p.theme.surface_hover : p.theme.surface_elevated;
                txt = p.theme.text;
                bd = p.theme.border;
            }
            int bw = btn_w(i);
            p.surface(bx, byy, bw, m.control_h, fill, m.radius_control);
            p.rrect_border(bx, byy, bw, m.control_h, m.radius_control, bd, 255);
            int tw = _ui_strlen(btns[i]) * glyph::ADVANCE;
            p.text(btns[i], bx + (bw - tw) / 2,
                   byy + text_vcenter(m.control_h), txt);
        }
    }
    void mark_btn(int i) {
        if (i < 0 || i >= n_btns) return;
        mark_area(btn_x(i), btn_row_y(), btn_w(i), Metrics().control_h);
    }
    virtual bool track_hover(int mx, int my) override {
        int i = hit_button(mx, my);
        if (i == hover_btn) return false;
        mark_btn(hover_btn);
        hover_btn = i;
        mark_btn(hover_btn);
        return true;
    }
    // out-of-class: butuh Window lengkap (close_dialog)
    virtual void on_click(int mx, int my) override;
};

} // namespace ui

#endif // KWIDGET_DIALOG_DIALOG_HPP
