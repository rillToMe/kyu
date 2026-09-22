// libdesktop backend — Canvas di atas libgui (adaptor tipis, tanpa salinan
// canvas, tanpa damage-tracking ganda).
//
// fill_rect/draw_text meneruskan ke gui_draw_* (libgui mencatat damage bbox
// internalnya sendiri); flush() = gui_flush() dan hanya dipanggil framework
// bila Shell melaporkan damage. gui_window_t hidup di Impl; aplikasi hanya
// melihat Canvas. Window dibuat + dihancurkan oleh Application, bukan Canvas.
#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/application.hpp>

extern "C" {
#include "libgui.h"
#include "userlib.h"
}

namespace kyuzen {
namespace desktop {

namespace {

// color_t libgui = struct per komponen (libs/color); konversi satu arah.
color_t to_gui_color(Color c) {
    color_t g;
    g.r = c.r;
    g.g = c.g;
    g.b = c.b;
    g.a = c.a ? c.a : 255;
    return g;
}

}  // namespace

struct Canvas::Impl {
    gui_window_t* win;
    Damage pending;
};

Canvas::Canvas() : impl_(0) {}

Canvas::~Canvas() {
    delete impl_;
    impl_ = 0;
}

void Canvas::attach(void* win) {
    delete impl_;
    impl_ = new Impl;
    impl_->win = static_cast<gui_window_t*>(win);
    impl_->pending = Damage::None;
}

int Canvas::width() const {
    return impl_ && impl_->win ? static_cast<int>(impl_->win->width) : 0;
}

int Canvas::height() const {
    return impl_ && impl_->win ? static_cast<int>(impl_->win->height) : 0;
}

void Canvas::fill_rect(const Rect& r, Color c) {
    if (!impl_ || !impl_->win) return;
    gui_draw_rect(impl_->win, r.x, r.y, r.width, r.height, to_gui_color(c));
}

void Canvas::draw_text(const char* text, Point p, Color c) {
    if (!impl_ || !impl_->win || !text) return;
    gui_draw_text(impl_->win, const_cast<char*>(text), p.x, p.y,
                  to_gui_color(c));
}

void Canvas::draw_px(int x, int y, const uint32_t* px, int w, int h) {
    if (!impl_ || !impl_->win || !px || w <= 0 || h <= 0) return;
    gui_window_t* win = impl_->win;
    int W = static_cast<int>(win->width);
    int H = static_cast<int>(win->height);
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > W ? W : x + w;
    int y1 = y + h > H ? H : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    // Posisi tulis ditentukan INDEKS kolom (dx), bukan penghitung piksel yang
    // berhasil ditulis: piksel transparan (a=0) dilewati tanpa menulis, jadi
    // pointer yang hanya naik saat menulis akan memadatkan sisa baris ke kiri
    // (tiap baris ikon bergeser sejauh jumlah piksel transparan di depannya ->
    // ikon tampak "meleot"/terpotong). Sumber dan tujuan memakai indeks yang
    // sama; hanya piksel yang benar-benar ditulis yang mengubah canvas.
    for (int dy = y0; dy < y1; dy++) {
        const uint32_t* srow = px + (dy - y) * w + (x0 - x);
        uint32_t* drow = win->canvas + dy * W + x0;
        int n = x1 - x0;
        for (int dx = 0; dx < n; dx++) {
            uint32_t s = srow[dx];
            uint32_t a = s >> 24;
            if (a == 0) continue;
            if (a == 255) {
                drow[dx] = s | 0xFF000000u;
                continue;
            }
            uint32_t d = drow[dx];
            uint32_t sr = (s >> 16) & 0xFFu, sg = (s >> 8) & 0xFFu,
                     sb = s & 0xFFu;
            uint32_t dr = (d >> 16) & 0xFFu, dg = (d >> 8) & 0xFFu,
                     db = d & 0xFFu;
            uint32_t na = 255u - a;
            uint32_t r = (sr * a + dr * na) / 255u;
            uint32_t g = (sg * a + dg * na) / 255u;
            uint32_t b = (sb * a + db * na) / 255u;
            drow[dx] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    gui_damage_rect(win, x0, y0, x1 - x0, y1 - y0);
}

void Canvas::mark_full() {
    if (impl_) impl_->pending = Damage::Full;
}

void Canvas::mark_region(const Rect& r) {
    (void)r;
    if (impl_ && impl_->pending == Damage::None)
        impl_->pending = Damage::Partial;
}

Damage Canvas::take_damage() {
    if (!impl_) return Damage::None;
    Damage d = impl_->pending;
    impl_->pending = Damage::None;
    return d;
}

void Canvas::flush() {
    if (impl_ && impl_->win) gui_flush(impl_->win);
}

}  // namespace desktop
}  // namespace kyuzen
