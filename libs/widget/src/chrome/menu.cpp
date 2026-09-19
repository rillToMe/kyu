// libs/widget/src/chrome/menu.cpp — dipindah apa adanya dari apps/libui.cpp.
#include "chrome/menu.hpp"
#include "window/window.hpp"

namespace ui {

void Menu::on_click(int mx, int my) {
    int idx = item_at(mx, my);       // pemisah & item disabled → -1 (tak bereaksi)
    if (idx >= 0) {
        ui_click_cb cb = items[idx].cb;
        void* d = items[idx].data;
        if (win) win->close_popup();
        if (cb) cb(d);
    }
}

} // namespace ui
