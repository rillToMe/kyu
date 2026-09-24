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
using kyuzen::desktop::Rect;
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

    // Akses uji (host test): state menu + seleksi ikon.
    bool menu_open() const { return menu_open_; }
    int menu_icon() const { return menu_icon_; }
    int menu_hover() const { return menu_hover_; }
    Rect menu_rect(int w, int h) const;
    int menu_count() const { return menu_icon_ >= 0 ? 2 : 3; }
    int menu_row_at(Point p, int w, int h) const;
    const Launcher& launcher() const { return launcher_; }

 private:
    Damage handle_click(Point p, int w, int h);
    Damage handle_move(Point p, int w, int h);
    Damage handle_right_click(Point p, int w, int h);
    // Canonical Hot Reload entry points (satu jalur untuk event LEGACY maupun
    // generik): baca config persisten → swap aman → Damage. Gagal = state lama
    // utuh, Damage::None.
    Damage reloadWallpaper(Canvas& canvas);
    Damage reloadFont();
    void menu_action(int row);
    void draw_menu(Canvas& canvas, int w, int h) const;
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
    // Context menu (top-most): terbuka via klik kanan, tutup via klik kiri
    // di luar / aksi item / Esc implisit (tak ada keyboard di shell).
    bool menu_open_;
    int menu_x_;     // titik klik (sebelum dijepit layar)
    int menu_y_;
    int menu_icon_;  // indeks ikon (menu app) atau -1 (menu desktop)
    int menu_hover_;  // baris hover, atau -1
    int drag_idx_;   // ikon sedang di-drag (free drag), atau -1
};

}  // namespace desktop_impl

#endif
