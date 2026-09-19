// libs/widget/include/dialog/promptdialog.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_DIALOG_PROMPTDIALOG_HPP
#define KWIDGET_DIALOG_PROMPTDIALOG_HPP

#include "dialog/dialog.hpp"
#include "core/painter.hpp"

namespace ui {

class Window;   // fwd: PromptDialog pegang Window* — TIDAK include window.hpp

// ------------------------------------------------------------
// PromptDialog — Dialog + satu kolom input teks (pengganti file dialog:
// File > Buka / Simpan Sebagai). Enter = OK (index 0), ESC/Batal = index 1.
//
// Jawaban dikirim lewat Window::close_prompt(): dialog DIHAPUS dulu, isi input
// disalin ke buffer lokal, baru callback ui_prompt_cb dijalankan. Urutan ini
// penting — callback bebas membuka dialog baru (mis. peringatan "belum
// disimpan") tanpa membuat dialog ini dihapus dua kali.
// ------------------------------------------------------------
class PromptDialog : public Dialog {
public:
    enum { MAX_INPUT = 255 };
    char input[MAX_INPUT + 1];
    int cur;                    // posisi kursor (indeks karakter)
    ui_prompt_cb pcb;
    void* pdata;

    PromptDialog(Window* owner_win, const char* t, const char* tx, const char* initial,
                 ui_prompt_cb c, void* d)
        : Dialog(owner_win, t, tx, prompt_btns(), 2, 0, 0), cur(0), pcb(c), pdata(d) {
        input[0] = '\0';
        if (initial) {
            int i = 0;
            for (; initial[i] && i < MAX_INPUT; i++) input[i] = initial[i];
            input[i] = '\0';
            cur = i;
        }
        h += 34;                 // ruang kolom input di atas baris tombol
        // Lebar minimal agar kolom input nyaman diketik (tombol "Batal").
        if (w < 300) w = 300;
    }
    static const char* const* prompt_btns() {
        static const char* b[2] = { "OK", "Batal" };
        return b;
    }
    int input_row_y() const { return btn_row_y() - 34; }
    int input_len() const { return _ui_strlen(input); }

    // Indeks kursor dari posisi x klik di kolom input.
    int input_at(int mx) const {
        int idx = (mx - (x + 18) + 4) / 8;
        if (idx < 0) idx = 0;
        int len = input_len();
        if (idx > len) idx = len;
        return idx;
    }
    void insert_ch(char ch) {
        int len = input_len();
        if (len >= MAX_INPUT) return;
        for (int i = len; i >= cur; i--) input[i + 1] = input[i];
        input[cur] = ch;
        cur++;
        mark_dirty();
    }
    void backspace() {
        if (cur <= 0) return;
        int len = input_len();
        for (int i = cur - 1; i < len; i++) input[i] = input[i + 1];
        cur--;
        mark_dirty();
    }
    virtual void draw(Painter& p) override {
        Dialog::draw(p);
        int iy = input_row_y();
        // Kolom input = kembali ke warna editor (paling gelap) + border halus.
        p.rect(x + 16, iy, w - 32, 24, p.theme.editor);
        p.rect(x + 16, iy, w - 32, 1, p.theme.mborder);
        p.rect(x + 16, iy + 23, w - 32, 1, p.theme.mborder);
        p.rect(x + 16, iy, 1, 24, p.theme.mborder);
        p.rect(x + w - 17, iy, 1, 24, p.theme.mborder);
        p.text(input, x + 22, iy + 4, p.theme.fg);
        p.rect(x + 22 + cur * 8, iy + 4, 2, 16, p.theme.caret);   // caret cyan
    }
    // out-of-class: butuh Window lengkap (close_prompt / close_dialog)
    virtual void on_cancel() override;
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override;
    virtual void on_click(int mx, int my) override;
};

} // namespace ui

#endif // KWIDGET_DIALOG_PROMPTDIALOG_HPP
