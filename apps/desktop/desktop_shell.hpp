// Kyuzen Desktop — shell default Phase 9: komposisi wallpaper + launcher +
// taskbar + preview + notice.
//
// Urutan gambar tiap render (kontrak lapisan): SEMUA ke canvas window desktop
// (Canvas::fill_rect/draw_text) — latar dari wallpaper(), lalu ikon launcher(),
// strip taskbar(), kartu preview(), terakhir notice(). Window desktop dibuat
// full-screen + opaque oleh libgui, sehingga gambar ke base_canvas
// (sys_draw_image) tidak pernah terlihat dan tidak dipakai lagi.
//
// Damage: Full = seluruh layar (wallpaper + ikon ulang); Partial = strip
// taskbar + kartu preview, dengan area bekas kartu dipulihkan dari wallpaper;
// gerak pointer di luar strip TANPA repaint.
#ifndef KYUZEN_DESKTOP_IMPL_DESKTOP_SHELL_HPP
#define KYUZEN_DESKTOP_IMPL_DESKTOP_SHELL_HPP

#include <kyuzen/desktop/shell.hpp>
#include "app_icons.hpp"
#include "app_preview.hpp"
#include "crash_notice.hpp"
#include "launcher.hpp"
#include "taskbar.hpp"
#include "wallpaper.hpp"

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
    Damage handle_move(Point p, int w, int h);
    void sync_preview(int w, int h);
    void render_full(Canvas& canvas, int w, int h);
    void render_partial(Canvas& canvas, int w, int h);

    Launcher launcher_;
    Taskbar taskbar_;
    CrashNotice notice_;
    AppPreview preview_;
    Wallpaper wallpaper_;
    IconCache icons_;
    WindowManager wm_;
    Point cursor_;
    uint64_t last_scan_ms_;
};

}  // namespace desktop_impl

#endif
