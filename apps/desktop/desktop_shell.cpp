// Kyuzen Desktop — implementasi shell Phase 9.
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

}  // namespace

DesktopShell::DesktopShell() : last_scan_ms_(0) {
    cursor_.x = 0;
    cursor_.y = 0;
}

void DesktopShell::on_start(Canvas& canvas) {
    int w = canvas.width();
    int h = canvas.height();
    launcher_.discover();
    print(const_cast<char*>("[desktop] layar "));
    print_num(static_cast<uint32_t>(w));
    print(const_cast<char*>("x"));
    print_num(static_cast<uint32_t>(h));
    print(const_cast<char*>(", app "));
    print_num(static_cast<uint32_t>(launcher_.count()));
    print(const_cast<char*>("\n"));
    if (wallpaper_.load(w, h)) {
        print(const_cast<char*>("[desktop] wallpaper: foto\n"));
    } else {
        print(const_cast<char*>("[desktop] wallpaper: gradasi\n"));
    }
    notice_.probe();  // kartu crash bila boot setelah panic
    last_scan_ms_ = System::uptime_ms();
    print(const_cast<char*>("[desktop] shell started (libdesktop)\n"));
}

Damage DesktopShell::handle_click(Point p, int w, int h) {
    // Klik di kartu preview: aktivasi window + tutup (dikonsumsi).
    if (preview_.hit(p)) {
        wm_.activate(preview_.window_id());
        preview_.hide();
        return Damage::Partial;
    }
    NoticeClick nc = notice_.on_click(p, w);
    if (nc == NoticeClick::CloseAndOpenFm) return Damage::Full;
    if (nc == NoticeClick::Close) {
        // Kartu tertutup tapi klik diteruskan ke aksi normal di bawah.
    }
    Damage d = (nc == NoticeClick::Close) ? Damage::Full : Damage::None;
    if (p.y >= h - TB_H) {
        int s = taskbar_.find_slot(p, w, h);
        if (s >= 0) wm_.activate(taskbar_.entry(taskbar_.slot_window(s)).id);
        if (preview_.visible()) preview_.hide();
        return Damage::Partial;
    }
    if (preview_.visible()) {
        preview_.hide();
        d = Damage::Partial;
    }
    int cols = Launcher::grid_cols(w);
    int cap = launcher_.grid_cap(w, h);
    int idx = launcher_.find_icon(p, cols, cap);
    if (idx >= 0) System::spawn(launcher_.entry(idx).elf);
    return d == Damage::Full ? Damage::Full : Damage::Partial;
}

void DesktopShell::sync_preview(int w, int h) {
    int s = taskbar_.hovered();
    if (s < 0 || s >= taskbar_.slots()) {
        preview_.hide();
        return;
    }
    int wi = taskbar_.slot_window(s);
    preview_.show(taskbar_.entry(wi), taskbar_.slot_app(s),
                  Taskbar::slot_rect(s, w, h), w, h);
}

Damage DesktopShell::handle_move(Point p, int w, int h) {
    cursor_ = p;
    if (p.y < h - TB_H) {
        // Di luar strip: hover mati + preview tutup (sekali).
        bool chg = taskbar_.update_hover(p, w, h);
        if (preview_.visible()) {
            preview_.hide();
            return Damage::Partial;
        }
        return chg ? Damage::Partial : Damage::None;
    }
    if (!taskbar_.update_hover(p, w, h)) return Damage::None;
    sync_preview(w, h);
    return Damage::Partial;
}

Damage DesktopShell::on_event(const Event& e, Canvas& canvas) {
    int w = canvas.width();
    int h = canvas.height();
    if (e.type == EventType::MouseMove) return handle_move(e.pos, w, h);
    if (e.type == EventType::MouseButton && e.button == 0 && e.pressed) {
        return handle_click(cursor_, w, h);
    }
    if (e.type == EventType::Quit) return Damage::None;
    return Damage::None;
}

Damage DesktopShell::on_poll(Canvas& canvas) {
    (void)canvas;
    Damage d = Damage::None;
    if (taskbar_.poll(wm_, launcher_)) d = Damage::Partial;
    // Preview menunjuk window yang mungkin hilang (tutup app): validasi.
    if (preview_.visible()) {
        bool alive = false;
        for (int i = 0; i < taskbar_.count(); i++) {
            if (taskbar_.entry(i).id == preview_.window_id()) {
                alive = true;
                break;
            }
        }
        if (!alive) {
            preview_.hide();
            d = Damage::Partial;
        }
    }
    uint64_t now = System::uptime_ms();
    if (now - last_scan_ms_ >= kRescanMs) {
        last_scan_ms_ = now;
        if (launcher_.discover()) d = Damage::Full;
    }
    if (notice_.update(now)) d = Damage::Full;
    return d;
}

void DesktopShell::render_full(Canvas& canvas, int w, int h) {
    // Satu arah: latar -> depan. Semuanya ke canvas window desktop (satu-
    // satunya permukaan yang terlihat; base_canvas dilewati compositor karena
    // window desktop full-screen + opaque).
    Rect all;
    all.x = 0;
    all.y = 0;
    all.width = w;
    all.height = h;
    wallpaper_.draw_bg(canvas, all, TB_H);
    int nl = launcher_.draw(canvas, icons_, w, h);
    int nt = taskbar_.draw(canvas, icons_, w, h);
    preview_.draw(canvas, icons_);
    // Diagnostik satu-shot (serial): berapa ikon yang benar-benar digambar.
    static bool diag_done = false;
    if (!diag_done) {
        diag_done = true;
        print(const_cast<char*>("[desktop] gambar: launcher "));
        print_num(static_cast<uint32_t>(nl));
        print(const_cast<char*>(", taskbar "));
        print_num(static_cast<uint32_t>(nt));
        print(const_cast<char*>("; wallpaper "));
        print_num(wallpaper_.has_image() ? 1u : 0u);
        print(const_cast<char*>("\n"));
    }
    notice_.draw(canvas);
}

void DesktopShell::render_partial(Canvas& canvas, int w, int h) {
    // Bekas kartu preview dipulihkan dari latar: kartu opaque tidak menghapus
    // dirinya sendiri saat pindah/hilang. Region kecil (kartu), bukan seluruh
    // layar — hover keluar-masuk tetap murah.
    Rect old = preview_.drawn_rect();
    if (old.width > 0 && old.height > 0)
        wallpaper_.draw_bg(canvas, old, TB_H);
    taskbar_.draw(canvas, icons_, w, h);
    preview_.draw(canvas, icons_);
}

void DesktopShell::render(Canvas& canvas, Damage d) {
    int w = canvas.width();
    int h = canvas.height();
    if (d == Damage::Full) {
        render_full(canvas, w, h);
    } else if (d == Damage::Partial) {
        render_partial(canvas, w, h);
    }
}

}  // namespace desktop_impl
