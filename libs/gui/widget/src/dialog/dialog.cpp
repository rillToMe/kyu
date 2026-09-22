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

} // namespace ui
