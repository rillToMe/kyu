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

DesktopShell::DesktopShell()
    : last_scan_ms_(0),
      menu_open_(false),
      menu_x_(0),
      menu_y_(0),
      menu_icon_(-1),
      menu_hover_(-1),
      drag_idx_(-1) {
    cursor_.x = 0;
    cursor_.y = 0;
}

void DesktopShell::on_start(Canvas& canvas) {
    int w = canvas.width();
    int h = canvas.height();
    launcher_.discover();
    launcher_.ui_font_init();  // UI font (Inter default; font.ui bila ada)
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
    // Menu terbuka = top-most: klik kiri di item = aksi, di luar = tutup.
    if (menu_open_) {
        int row = menu_row_at(p, w, h);
        if (row >= 0) menu_action(row);
        menu_open_ = false;
        menu_hover_ = -1;
        return Damage::Full;
    }
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
    // (h = 0 hanya di host test — canvas null; strip tak ada di sana.)
    if (h > 0 && p.y >= h - TB_H) {
        int s = taskbar_.find_slot(p, w, h);
        if (s >= 0) wm_.activate(taskbar_.entry(taskbar_.slot_window(s)).id);
        if (preview_.visible()) preview_.hide();
        return Damage::Partial;
    }
    if (preview_.visible()) {
        preview_.hide();
        d = Damage::Partial;
    }
    int idx = launcher_.find_icon_at(p, w, h);
    if (idx < 0) {
        // Klik area kosong: batalkan seleksi (ala Windows).
        if (launcher_.selected() >= 0) {
            launcher_.set_selected(-1);
            return Damage::Full;
        }
        return d == Damage::Full ? Damage::Full : Damage::Partial;
    }
    if (launcher_.selected() != idx) {
        // Klik pertama = seleksi saja; klik kedua (sudah terseleksi) = buka.
        launcher_.set_selected(idx);
        // Free drag dimulai dari ikon terseleksi saat auto-arrange mati.
        if (!launcher_.auto_arrange()) drag_idx_ = idx;
        return Damage::Full;
    }
    drag_idx_ = -1;
    System::spawn(launcher_.entry(idx).elf);
    return d == Damage::Full ? Damage::Full : Damage::Partial;
}

Damage DesktopShell::handle_right_click(Point p, int w, int h) {
    // Klik kanan di strip taskbar / kartu notice: bukan menu desktop.
    // (h = 0 hanya di host test — canvas null; strip tak ada di sana.)
    if (h > 0 && p.y >= h - TB_H) return Damage::None;
    if (preview_.hit(p)) return Damage::None;
    if (notice_.visible()) return Damage::None;
    menu_icon_ = launcher_.find_icon_at(p, w, h);
    if (menu_icon_ >= 0) launcher_.set_selected(menu_icon_);
    menu_x_ = p.x;
    menu_y_ = p.y;
    menu_hover_ = -1;
    menu_open_ = true;
    return Damage::Full;
}

// Item desktop: 0 = Auto Arrange Icons (centang), 1 = Sort by Name,
// 2 = Refresh. Item ikon: 0 = Open, 1 = Properties (tutup saja).
void DesktopShell::menu_action(int row) {
    if (menu_icon_ >= 0) {
        if (row == 0 && menu_icon_ < launcher_.count()) {
            drag_idx_ = -1;
            System::spawn(launcher_.entry(menu_icon_).elf);
        }
        return;  // Properties / lainnya: tutup tanpa aksi
    }
    if (row == 0) {
        launcher_.set_auto_arrange(!launcher_.auto_arrange());
        drag_idx_ = -1;
    } else if (row == 1) {
        launcher_.sort_by_name();
        drag_idx_ = -1;
    } else if (row == 2) {
        launcher_.discover();  // refresh: pindai ulang /apps
        drag_idx_ = -1;
    }
}

Rect DesktopShell::menu_rect(int w, int h) const {
    Rect r;
    r.width = MENU_W;
    r.height = menu_count() * MENU_ROW_H + MENU_PAD * 2;
    int x = menu_x_;
    int y = menu_y_;
    if (x + r.width + 4 > w) x = w - r.width - 4;
    if (y + r.height + 4 > h - TB_H) y = h - TB_H - r.height - 4;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    r.x = x;
    r.y = y;
    return r;
}

int DesktopShell::menu_row_at(Point p, int w, int h) const {
    if (!menu_open_) return -1;
    Rect r = menu_rect(w, h);
    if (!r.contains(p)) return -1;
    int row = (p.y - (r.y + MENU_PAD)) / MENU_ROW_H;
    if (row < 0 || row >= menu_count()) return -1;
    return row;
}

void DesktopShell::draw_menu(Canvas& canvas, int w, int h) const {
    if (!menu_open_) return;
    Rect r = menu_rect(w, h);
    canvas.fill_rect(r, MENU_BG);
    Rect e;
    e.x = r.x;
    e.y = r.y;
    e.width = r.width;
    e.height = 1;
    canvas.fill_rect(e, MENU_EDGE);
    e.y = r.y + r.height - 1;
    canvas.fill_rect(e, MENU_EDGE);
    e.x = r.x;
    e.y = r.y;
    e.width = 1;
    e.height = r.height;
    canvas.fill_rect(e, MENU_EDGE);
    e.x = r.x + r.width - 1;
    canvas.fill_rect(e, MENU_EDGE);
    for (int i = 0; i < menu_count(); i++) {
        int ry = r.y + MENU_PAD + i * MENU_ROW_H;
        if (i == menu_hover_) {
            Rect hr;
            hr.x = r.x + 1;
            hr.y = ry;
            hr.width = r.width - 2;
            hr.height = MENU_ROW_H;
            canvas.fill_rect(hr, MENU_HOVER);
        }
        // Centang "Auto Arrange Icons" saat aktif.
        if (menu_icon_ < 0 && i == 0 && launcher_.auto_arrange()) {
            Rect ck;
            ck.x = r.x + 8;
            ck.y = ry + (MENU_ROW_H - 8) / 2;
            ck.width = 8;
            ck.height = 8;
            canvas.fill_rect(ck, MENU_ACC);
        }
        const char* txt = "";
        if (menu_icon_ >= 0) {
            txt = (i == 0) ? "Open" : "Properties";
        } else {
            if (i == 0)
                txt = "Auto Arrange Icons";
            else if (i == 1)
                txt = "Sort by Name";
            else
                txt = "Refresh";
        }
        Point tp;
        tp.x = r.x + 24;
        tp.y = ry + (MENU_ROW_H - 16) / 2;
        canvas.draw_text(txt, tp, MENU_TXT);
    }
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
    // Free drag: ikon terseleksi mengikuti kursor (dijepit area desktop).
    if (drag_idx_ >= 0 && !launcher_.auto_arrange() &&
        drag_idx_ < launcher_.count()) {
        int nx = p.x - BOX_W / 2;
        int ny = p.y - BOX_ICON_Y - ICON_SZ / 2;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        // Jepit atas hanya bila layar muat (w/h = 0 di host test).
        if (w > BOX_W && nx > w - BOX_W) nx = w - BOX_W;
        if (h - TB_H > BOX_H && ny > h - TB_H - BOX_H) ny = h - TB_H - BOX_H;
        launcher_.set_custom_pos(drag_idx_, nx, ny);
        return Damage::Full;
    }
    // Hover baris menu saat menu terbuka (menu = top-most).
    if (menu_open_) {
        int row = menu_row_at(p, w, h);
        if (row != menu_hover_) {
            menu_hover_ = row;
            return Damage::Full;
        }
        return Damage::None;
    }
    // Hover box ikon desktop (di luar strip taskbar; h = 0 di host test
    // berarti seluruh layar = area desktop).
    bool in_strip = (h > 0 && p.y >= h - TB_H);
    if (!in_strip) {
        int idx = launcher_.find_icon_at(p, w, h);
        bool chg = (idx != launcher_.hovered());
        if (chg) launcher_.set_hover(idx);
        // Di luar strip: hover taskbar mati + preview tutup (sekali).
        bool tch = taskbar_.update_hover(p, w, h);
        if (preview_.visible()) {
            preview_.hide();
            return Damage::Partial;
        }
        if (chg) return Damage::Full;
        return tch ? Damage::Partial : Damage::None;
    }
    if (launcher_.hovered() >= 0) launcher_.set_hover(-1);
    if (!taskbar_.update_hover(p, w, h)) return Damage::None;
    sync_preview(w, h);
    return Damage::Partial;
}

Damage DesktopShell::reloadWallpaper(Canvas& canvas) {
    // Syscall 84 LEGACY maupun 85/HOT_RELOAD(WALLPAPER) tiba di sini: muat
    // ulang dari konfigurasi persisten di loop normal (bukan konteks syscall).
    // poll() swap aman: gagal = wallpaper lama tetap. Request beruntun aman:
    // tiap reload membaca konfigurasi TERKINI, jadi akhirnya tampil yang
    // terakhir (diproses satu per satu via antrean event).
    if (wallpaper_.poll(canvas.width(), canvas.height())) {
        print(const_cast<char*>("[desktop] wallpaper: ganti\n"));
        return Damage::Full;
    }
    return Damage::None;
}

Damage DesktopShell::reloadFont() {
    // HOT_RELOAD(FONT): jalur event-driven yang sama dengan poll berkala di
    // on_poll (dipertahankan sebagai jaring pengaman untuk perubahan di luar
    // API). ui_font_load swap aman: gagal = font lama tetap.
    if (launcher_.ui_font_poll()) {
        return Damage::Full;
    }
    return Damage::None;
}

Damage DesktopShell::on_event(const Event& e, Canvas& canvas) {
    int w = canvas.width();
    int h = canvas.height();
    if (e.type == EventType::MouseMove) return handle_move(e.pos, w, h);
    if (e.type == EventType::MouseButton && e.pressed) {
        if (e.button == 1) return handle_right_click(cursor_, w, h);
        if (e.button == 0) return handle_click(cursor_, w, h);
        return Damage::None;
    }
    if (e.type == EventType::MouseButton && !e.pressed && e.button == 0) {
        // Lepas kiri = akhir free drag (klik tanpa gerak = tanpa kerja).
        if (drag_idx_ >= 0) {
            drag_idx_ = -1;
            return Damage::Full;
        }
        return Damage::None;
    }
    if (e.type == EventType::Quit) return Damage::None;
    if (e.type == EventType::WallpaperReload) {
        // LEGACY syscall 84 — satu jalur kanonis dengan HOT_RELOAD(WALLPAPER).
        return reloadWallpaper(canvas);
    }
    if (e.type == EventType::HotReload) {
        // Syscall 85 generik: dispatch per target ke owner handler. Target tak
        // dikenal di sisi-desktop (mis. nilai masa depan) = abaikan aman.
        if (e.hot_target == KZ_HOT_RELOAD_WALLPAPER)
            return reloadWallpaper(canvas);
        if (e.hot_target == KZ_HOT_RELOAD_FONT)
            return reloadFont();
        return Damage::None;
    }
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
        // Font UI live (≤5 dtk setelah Settings menyimpan): reload +
        // Full redraw. Tanpa reboot/restart (damage existing).
        if (launcher_.ui_font_poll()) d = Damage::Full;
        // Wallpaper TIDAK di-poll di sini: reload eksplisit via event
        // WallpaperReload (syscall 84) di on_event — seketika, tanpa
        // decode berulang tiap rescan. Startup tetap via load() di on_start.
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
    draw_menu(canvas, w, h);  // top-most: selalu di atas ikon/taskbar/notice
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
    // Menu terbuka = lapisan ikon ikut berubah: Partial tak cukup.
    if (menu_open_ && d == Damage::Partial) d = Damage::Full;
    if (d == Damage::Full) {
        render_full(canvas, w, h);
    } else if (d == Damage::Partial) {
        render_partial(canvas, w, h);
    }
}

}  // namespace desktop_impl
