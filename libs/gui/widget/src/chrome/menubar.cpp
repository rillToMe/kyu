// libs/widget/src/chrome/menubar.cpp — dipindah apa adanya dari apps/libui.cpp.
#include "chrome/menubar.hpp"
#include "window/window.hpp"

namespace ui {

void MenuBar::on_click(int mx, int my) {
    (void)my;                       // judul menu satu baris; cukup x
    int i = title_at(mx);
    if (i < 0) { if (win) win->close_popup(); return; }
    if (win && win->popup == titles[i].menu) { win->close_popup(); return; }
    if (win) win->open_popup(titles[i].menu, title_x(i), y + h);
}
void MenuBar::draw(Painter& p) {
    // Menubar = permukaan "chrome" (#2D2D2D) + divider 1px di bawahnya —
    // beda lapisan dari area teks (#1E1E1E) supaya hierarki UI terlihat.
    p.rect(x, y, w, h, p.theme.chrome);
    p.rect(x, y + h - 1, w, 1, p.theme.divider);
    for (int i = 0; i < n; i++) {
        int tx = title_x(i), tw = title_w(i);
        bool open = win && win->popup == titles[i].menu;
        if (open || i == hover_idx) p.rect(tx, y + 1, tw, h - 2, p.theme.button_hover);
        p.text(titles[i].label, tx + 11, y + 4, p.theme.button_fg);
    }
}

} // namespace ui
