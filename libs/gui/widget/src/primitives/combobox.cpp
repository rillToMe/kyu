// libs/widget/src/primitives/combobox.cpp — definisi butuh-Window combobox.hpp.
#include "primitives/combobox.hpp"
#include "window/window.hpp"

namespace ui {

ComboBox::~ComboBox() {
    for (int i = 0; i < n; i++) _ui_free(items[i]);
    free_picks();
    if (menu) {
        // Popup terbuka saat dihancurkan → tutup dulu (Window::popup
        // tak boleh dangling). Hook mengembalikan open=false.
        if (win && win->popup == menu) win->close_popup();
        delete menu;
    }
}

void ComboBox::dismiss() {
    if (!open) return;
    if (win) {
        // Damage area popup SEBELUM tutup (tanpa ini commit keyboard
        // meninggalkan ghost: jalur mouse/ESC/Tab sudah damage di pemanggil).
        if (menu) win->damage_overlay(menu);
        win->close_popup();   // hook mengembalikan open=false
    } else { open = false; free_picks(); }
}

void ComboBox::open_menu() {
    if (!enabled || open || !win || n <= 0) return;
    delete menu;
    menu = new Menu(win);
    n_picks = 0;
    for (int i = 0; i < n; i++) {
        picks[n_picks].self = this;
        picks[n_picks].idx = i;
        menu->add_item(items[i], pick_trampoline, &picks[n_picks]);
        n_picks++;
    }
    if (selected >= 0 && selected < n) menu->hover_idx = selected;
    open = true;
    win->open_popup(menu, x, y + h);
    win->popup_owner = this;
    mark_dirty();
}

} // namespace ui
