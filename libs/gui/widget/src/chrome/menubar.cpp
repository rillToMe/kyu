// libs/widget/src/chrome/menubar.cpp — gambar + klik MenuBar.
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
    const TypeRole& role = p.theme.type.label;
    // Menubar = permukaan chrome (satu level dari halaman) + garis 1px di
    // bawahnya. Bukan kotak per judul: bar terasa sebagai satu pita.
    p.rect(x, y, w, h, p.theme.chrome);
    p.rect(x, y + h - 1, w, 1, p.theme.border_subtle);
    for (int i = 0; i < n; i++) {
        int tx = title_x(i), tw = title_w(i);
        bool open = win && win->popup == titles[i].menu;
        // Judul terbuka/hover: latar tipis dengan inset 2px supaya tidak
        // menempel ke tepi bar (bar tidak boleh terlihat "berkotak").
        if (open || i == hover_idx)
            p.rect(tx + 2, y + 2, tw - 4, h - 5, p.theme.surface_hover);
        color_t tc = (open || i == hover_idx) ? p.theme.text
                                              : p.theme.text_secondary;
        int tl = _ui_strlen(titles[i].label) * glyph::ADVANCE;
        p.text(titles[i].label, tx + (tw - tl) / 2, y + text_vcenter(h), tc);
    }
}

} // namespace ui
