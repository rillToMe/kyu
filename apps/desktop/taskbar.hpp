// Kyuzen Desktop — taskbar: daftar window + tombol aktivasi.
//
// Kebijakan implementasi: query via WindowManager framework, filter
// (lewati window desktop + tanpa judul), change-detection untuk damage
// Partial. Judul 32 char = batas ABI KWM.
#ifndef KYUZEN_DESKTOP_IMPL_TASKBAR_HPP
#define KYUZEN_DESKTOP_IMPL_TASKBAR_HPP

#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/geometry.hpp>
#include <kyuzen/desktop/window_manager.hpp>

namespace desktop_impl {

using kyuzen::desktop::Canvas;
using kyuzen::desktop::Point;
using kyuzen::desktop::Rect;
using kyuzen::desktop::WindowInfo;
using kyuzen::desktop::WindowManager;

const int MAX_WINS = 16;

class Taskbar {
public:
    Taskbar();

    // Query ulang via wm; true = tampilan berubah (perlu damage Partial).
    bool poll(WindowManager& wm);
    int count() const { return nwins_; }
    const WindowInfo& entry(int i) const { return wins_[i]; }

    // Tombol di titik p (pemanggil memfilter strip taskbar dulu), atau -1.
    int find_button(Point p) const;

    void draw(Canvas& canvas) const;

private:
    WindowInfo wins_[MAX_WINS];
    int nwins_;
};

}  // namespace desktop_impl

#endif
