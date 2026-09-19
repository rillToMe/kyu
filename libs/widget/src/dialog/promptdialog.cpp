// libs/widget/src/dialog/promptdialog.cpp — dipindah apa adanya dari apps/libui.cpp.
#include "dialog/promptdialog.hpp"
#include "window/window.hpp"

namespace ui {

// Out-of-class PromptDialog (butuh Window lengkap).

void PromptDialog::on_cancel() {
    if (win) win->close_prompt(this, 0);   // ESC / tombol Batal → batal
}
void PromptDialog::on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) {
    (void)mods;
    if (ascii >= 32 && ascii < 127) { insert_ch((char)ascii); return; }
    int len = input_len();
    switch (scancode) {
        case 0x0E: backspace(); break;                                  // Backspace
        case 0x1C: if (win) win->close_prompt(this, 1); break;           // Enter = OK
        case 0x4B: if (cur > 0) { cur--; mark_dirty(); } break;          // ←
        case 0x4D: if (cur < len) { cur++; mark_dirty(); } break;        // →
        case 0x47: cur = 0; mark_dirty(); break;                         // Home
        case 0x4F: cur = len; mark_dirty(); break;                       // End
        default: break;
    }
}
void PromptDialog::on_click(int mx, int my) {
    int i = hit_button(mx, my);
    if (i >= 0) { if (win) win->close_prompt(this, i == 0); return; }
    int iy = input_row_y();
    if (my >= iy && my < iy + 24 && mx >= x + 12 && mx < x + w - 12) {
        cur = input_at(mx);        // klik di kolom input → pindahkan kursor
        mark_dirty();
    }
}


} // namespace ui
