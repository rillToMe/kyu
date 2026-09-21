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
