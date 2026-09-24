// libs/widget/include/primitives/fttext.hpp — area teks FreeType (callback).
#ifndef KWIDGET_PRIMITIVES_FTTEXT_HPP
#define KWIDGET_PRIMITIVES_FTTEXT_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

// ------------------------------------------------------------
// FtText — persegi teks yang digambar PEMANGGIL via callback.
//
// Toolkit TIDAK me-link FreeType: draw() hanya memberi akses canvas
// + memanggil balik app. App memakai kz_text_draw (libs/text) di
// dalam callback, lalu damage diurus mekanisme widget ini
// (refresh() -> mark_dirty -> Window::render -> gui_flush).
//
// Kontrak: app mengukur teks (kz_text_measure) dan memilih ukuran
// widget yang memuatnya; luapan di luar rect di-clip ke canvas saja.
// Identical-repaint aman: menggambar ulang piksel yang sama di luar
// dirty rect tidak merusak apa pun (damage upload = union).
// ------------------------------------------------------------
class FtText : public Widget {
public:
    ui_fttext_draw_cb draw_cb;
    void* draw_data;
    FtText(int w_, int h_) : draw_cb(0), draw_data(0) { w = w_; h = h_; }
    void set_draw(ui_fttext_draw_cb cb, void* u) {
        draw_cb = cb; draw_data = u;
        mark_dirty();
    }
    void refresh() { mark_dirty(); }
    virtual void draw(Painter& p) override {
        if (!draw_cb || !visible) return;
        int cx = x, cy = y, cw = w, ch = h;
        if (!p.clip_rect(cx, cy, cw, ch)) return;   // tak terlihat -> skip
        draw_cb(draw_data, p.win->canvas,
                (int)p.win->width, (int)p.win->height, x, y);
        // Callback menulis canvas LANGSUNG (kz_text_draw, bukan primitif
        // libgui) sehingga tak ada damage yang tercatat — tandai rect
        // widget eksplisit agar gui_flush meng-upload (tanpa ini layar
        // menyimpan piksel lama walau canvas sudah baru).
        gui_damage_rect(p.win, x, y, w, h);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_FTTEXT_HPP
