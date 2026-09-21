// Kyuzen Desktop — implementasi shell (port desktop generasi C).
//
// Damage dijaga seperti desktop lama: Full awal + saat grid/notice berubah;
// Partial (taskbar saja) saat klik/fokus/daftar berubah; gerak pointer murni
// TANPA repaint (tak ada hover state).
#include "desktop_shell.hpp"
#include "sys_abi.hpp"
#include "theme.hpp"

namespace desktop_impl {

using kyuzen::desktop::Damage;
using kyuzen::desktop::Event;
using kyuzen::desktop::EventType;
using kyuzen::desktop::Rect;
using kyuzen::desktop::System;

namespace {

const uint64_t kRescanMs = 5000;  // app baru di FS muncul ≤5 dtk

void draw_wallpaper(Canvas& canvas, const Launcher& launcher) {
    int W = canvas.width();
    int H = canvas.height();
    Rect bg;
    bg.x = 0;
    bg.y = 0;
    bg.width = W;
    bg.height = H - TB_H;
    canvas.fill_rect(bg, WALL_BG);
    const char* brand = "KyuzenOS";
    int n = 0;
    while (brand[n]) n++;
    Point p;
    p.x = W - n * 8 - 16;
    p.y = 12;
    canvas.draw_text(brand, p, WALL_TXT);
    launcher.draw(canvas);
}

}  // namespace

DesktopShell::DesktopShell() : last_scan_ms_(0) {
    cursor_.x = 0;
    cursor_.y = 0;
}

void DesktopShell::on_start(Canvas& canvas) {
    (void)canvas;
    launcher_.discover();
    notice_.probe();  // kartu crash bila boot setelah panic
    last_scan_ms_ = System::uptime_ms();
    print(const_cast<char*>("[desktop] shell started (libdesktop)\n"));
}

Damage DesktopShell::handle_click(Point p, int w, int h) {
    NoticeClick nc = notice_.on_click(p, w);
    if (nc == NoticeClick::CloseAndOpenFm) return Damage::Full;
    if (nc == NoticeClick::Close) {
        // Kartu tertutup tapi klik diteruskan ke aksi normal di bawah.
    }
    Damage d = (nc == NoticeClick::Close) ? Damage::Full : Damage::None;
    if (p.y >= h - TB_H) {
        int b = taskbar_.find_button(p);
        if (b >= 0) wm_.activate(taskbar_.entry(b).id);
        return Damage::Partial;
    }
    int cols = Launcher::grid_cols(w);
    int cap = launcher_.grid_cap(w, h);
    int idx = launcher_.find_icon(p, cols, cap);
    if (idx >= 0) System::spawn(launcher_.entry(idx).elf);
    return d == Damage::Full ? Damage::Full : Damage::Partial;
}

Damage DesktopShell::on_event(const Event& e, Canvas& canvas) {
    int w = canvas.width();
    int h = canvas.height();
    if (e.type == EventType::MouseMove) {
        cursor_ = e.pos;
        return Damage::None;
    }
    if (e.type == EventType::MouseButton && e.button == 0 && e.pressed) {
        return handle_click(cursor_, w, h);
    }
    if (e.type == EventType::Quit) return Damage::None;
    return Damage::None;
}

Damage DesktopShell::on_poll(Canvas& canvas) {
    (void)canvas;
    Damage d = Damage::None;
    if (taskbar_.poll(wm_)) d = Damage::Partial;
    uint64_t now = System::uptime_ms();
    if (now - last_scan_ms_ >= kRescanMs) {
        last_scan_ms_ = now;
        if (launcher_.discover()) d = Damage::Full;
    }
    if (notice_.update(now)) d = Damage::Full;
    return d;
}

void DesktopShell::render(Canvas& canvas, Damage d) {
    if (d == Damage::Full) {
        draw_wallpaper(canvas, launcher_);
        taskbar_.draw(canvas);
        notice_.draw(canvas);
    } else if (d == Damage::Partial) {
        taskbar_.draw(canvas);
    }
}

}  // namespace desktop_impl
