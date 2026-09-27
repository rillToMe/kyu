// libs/widget/src/dialog/dialog.cpp — dipindah apa adanya dari apps/libui.cpp.
#include "dialog/dialog.hpp"
#include "window/window.hpp"

namespace ui {

// Out-of-class: butuh Window lengkap (popup handling).
void Dialog::on_click(int mx, int my) {
    int i = hit_button(mx, my);
    if (i < 0) return;              // klik body dialog → tetap modal, abaikan
    ui_dialog_cb c = cb;
    void* d = data;
    if (win) win->close_dialog();   // hapus dialog dulu (delete this)
    if (c) c(d, i);                 // lalu fire cb — jangan sentuh member lagi
}

// Phase C: navigasi tombol via keyboard (dialog biasa; PromptDialog punya
// on_key sendiri untuk input teks).
void Dialog::on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) {
    (void)mods;
    if (n_btns <= 0) return;
    mark_dirty();
    uint32_t sc = scancode & 0xFF;
    if (sc == 0x4B) {                        // Left: hover mundur
        int n = hover_btn < 0 ? 0 : (hover_btn + n_btns - 1) % n_btns;
        mark_btn(hover_btn); hover_btn = n; mark_btn(hover_btn);
        return;
    }
    if (sc == 0x4D) {                        // Right: hover maju
        int n = hover_btn < 0 ? 0 : (hover_btn + 1) % n_btns;
        mark_btn(hover_btn); hover_btn = n; mark_btn(hover_btn);
        return;
    }
    if (ascii == '\n' || ascii == '\r' || ascii == ' ' || sc == 0x1C) {
        int i = hover_btn >= 0 ? hover_btn : 0;
        if (i < n_btns) on_click(btn_x(i) + 2, btn_row_y() + 2);
    }
}

} // namespace ui
