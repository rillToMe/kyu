// Kyuzen Desktop — shell default: komposisi launcher + taskbar + notice.
//
// Memiliki: wallpaper, state desktop (posisi kursor, jadwal re-scan),
// koordinasi render (Full = semuanya; Partial = taskbar saja — pemetaan
// Damage generik ke kebijakan desktop ini milik shell, bukan framework).
#ifndef KYUZEN_DESKTOP_IMPL_DESKTOP_SHELL_HPP
#define KYUZEN_DESKTOP_IMPL_DESKTOP_SHELL_HPP

#include <kyuzen/desktop/shell.hpp>
#include "crash_notice.hpp"
#include "launcher.hpp"
#include "taskbar.hpp"

namespace desktop_impl {

using kyuzen::desktop::Canvas;
using kyuzen::desktop::Damage;
using kyuzen::desktop::Event;
using kyuzen::desktop::Point;
using kyuzen::desktop::Shell;
using kyuzen::desktop::WindowManager;

class DesktopShell : public Shell {
public:
    DesktopShell();

    void on_start(Canvas& canvas) override;
    Damage on_event(const Event& e, Canvas& canvas) override;
    Damage on_poll(Canvas& canvas) override;
    void render(Canvas& canvas, Damage d) override;
    bool is_running() const override { return true; }

private:
    Damage handle_click(Point p, int w, int h);

    Launcher launcher_;
    Taskbar taskbar_;
    CrashNotice notice_;
    WindowManager wm_;
    Point cursor_;
    uint64_t last_scan_ms_;
};

}  // namespace desktop_impl

#endif
