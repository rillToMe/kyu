// libs/widget/include/dialog/dialog.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_DIALOG_DIALOG_HPP
#define KWIDGET_DIALOG_DIALOG_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "runtime/memory.hpp"

namespace ui {

class Window;   // fwd: Dialog pegang Window* — TIDAK include window.hpp

// ------------------------------------------------------------
// Dialog — overlay modal tengah-window (Phase 9). Non-blocking: cb(index)
// dipanggil saat tombol ditekan, index = -1 bila dibatalkan (ESC).
// Klik di luar dialog diabaikan (modal memblok input latar).
//
// Aksen teks: baris isi yang DIAWALI prefix UI_ACCENT_PREFIX digambar dengan
// warna theme.acc_text (amber) — aplikasi menandai baris shortcut ("Ctrl+N")
// agar menonjol dari deskripsi fungsinya (gaya hint VS Code).
// ------------------------------------------------------------
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
        // Ukuran dari isi: elemen terpanjang, min 220px.
        int nlines = 1;
        for (int i = 0; text[i]; i++) if (text[i] == '\n') nlines++;
        int wid = 220;
        int cand = _ui_strlen(title) * 8 + 24;
        if (cand > wid) wid = cand;
        int start = 0, i = 0;
        for (;;) {
            if (text[i] == '\n' || text[i] == '\0') {
                int len = (i - start) * 8 + 24;
                if (len > wid) wid = len;
                if (text[i] == '\0') break;
                start = i + 1;
            }
            i++;
        }
        int bw = 0;
        for (int i = 0; i < n_btns; i++) bw += btn_w(i) + 6;
        if (bw - 6 + 48 > wid) wid = bw - 6 + 48;
        // Lebar min 300: teks tidak menempel ke border (padding kiri/kanan
        // 16px) dan judul punya ruang napas.
        if (wid < 300) wid = 300;
        this->w = wid;
        this->h = 14 + 16 + 6 + nlines * 16 + 12 + 28 + 14;
    }
    virtual ~Dialog() {
        _ui_free(title); _ui_free(text);
        for (int i = 0; i < MAX_BTNS; i++) _ui_free(btns[i]);
    }
    int btn_row_y() const { return y + h - 42; }
    int btn_total() const {
        int t = 0;
        for (int i = 0; i < n_btns; i++) t += btn_w(i) + 6;
        return t - 6;
    }
    int btn_x(int i) const {
        int bx = x + (w - btn_total()) / 2;
        for (int j = 0; j < i; j++) bx += btn_w(j) + 6;
        return bx;
    }
    int btn_w(int i) const { return _ui_strlen(btns[i]) * 8 + 20; }
    int hit_button(int mx, int my) const {
        if (my < btn_row_y() || my >= btn_row_y() + 28) return -1;
        for (int i = 0; i < n_btns; i++)
            if (mx >= btn_x(i) && mx < btn_x(i) + btn_w(i)) return i;
        return -1;
    }
    // Dipanggil Window saat ESC menutup dialog, SEBELUM objek ini dihapus.
    // PromptDialog memakainya untuk mengirim "dibatalkan" ke aplikasi.
    virtual void on_cancel() {}
    virtual void draw(Painter& p) override {
        // Modal = permukaan "panel" (#252526) + border halus (#454545-ish) +
        // garis aksen tipis di tepi atas (identitas dialog, senada titlebar
        // modern) + divider di atas baris tombol.
        p.rect(x, y, w, h, p.theme.panel);
        p.rect(x, y, w, 2, p.theme.accent);
        p.rect(x, y, w, 1, p.theme.mborder);
        p.rect(x, y + h - 1, w, 1, p.theme.mborder);
        p.rect(x, y, 1, h, p.theme.mborder);
        p.rect(x + w - 1, y, 1, h, p.theme.mborder);
        p.text(title, x + 16, y + 14, p.theme.button_fg);       // judul putih
        // Isi: baris ber-prefix aksen digambar dgn warna acc_text (amber).
        {
            int ty = y + 36, start = 0, i = 0;
            for (;;) {
                if (text[i] == '\n' || text[i] == '\0') {
                    bool acc = (_ui_strncmp(text + start, UI_ACCENT_PREFIX,
                                            UI_ACCENT_PREFIX_LEN) == 0);
                    char save = text[i];
                    // p.text() menggambar sampai NUL — pinjam byte baris itu.
                    const_cast<char*>(text)[i] = '\0';
                    p.text(text + start, x + 16, ty,
                           acc ? p.theme.acc_text : p.theme.fg);
                    const_cast<char*>(text)[i] = save;
                    if (save == '\0') break;
                    start = i + 1;
                    ty += 16;
                }
                i++;
            }
        }
        p.rect(x, btn_row_y() - 8, w, 1, p.theme.divider);
        int by = btn_row_y();
        for (int i = 0; i < n_btns; i++) {
            int bx = btn_x(i);
            p.rect(bx, by, btn_w(i), 28,
                   i == hover_btn ? p.theme.button_hover : p.theme.btnfill);
            p.text(btns[i], bx + 10, by + 6, p.theme.button_fg);
        }
    }
    void mark_btn(int i) {
        if (i < 0 || i >= n_btns) return;
        mark_area(btn_x(i), btn_row_y(), btn_w(i), 28);
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
