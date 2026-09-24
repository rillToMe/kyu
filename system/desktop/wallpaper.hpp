// Kyuzen Desktop — wallpaper: foto + fallback prosedural.
//
// Sumber konfigurasi = manifest desktop sendiri ("/apps/desktop.app",
// kunci "wallpaper=<nama>"); tanpa aplikasi settings (Phase 9 hanya
// infrastruktur + presentasi sisi-desktop).
//
// Rantai fallback: pilihan -> WALL_DEFAULT -> gradasi prosedural.
// Foto (PNG, assets/wallpaper/ via modul limine di akar FS; decode lewat
// png_decode bersama libs/media/png.c) di-decode SEKALI (load), diskala ke ukuran
// layar, lalu digambar ke CANVAS WINDOW desktop (RLE fill_rect per baris —
// libgui tak punya draw-image). Gradasi fallback juga prosedural (strip
// fill_rect) sehingga selalu ada latar.
//
// Catatan penting (bug Phase 9): gambar ke base_canvas via sys_draw_image
// TIDAK pernah terlihat. Window desktop dibuat full-screen oleh libgui dan
// langsung diisi latar opaque + dideklarasikan opaque, sehingga compositor
// (a) melewatkan base-blit untuk seluruh layar dan (b) menimpa base dengan
// canvas window. Semua pixel yang harus terlihat digambar ke canvas window.
//
// Render Partial memakai region: area yang ditinggalkan kartu preview
// dipulihkan dari wallpaper (bukan render ulang seluruh layar).
//
// Live reload: Wallpaper mengingat pilihan manifest terakhir (sel_). poll()
// membaca ulang kunci "wallpaper=" dan, HANYA bila berubah, memuat gambar baru
// lewat loadSelection() (buffer baru dialokasikan dulu; yang lama diganti
// hanya bila yang baru siap — gagal muat = wallpaper lama tetap). Dipanggil
// DesktopShell::on_poll tiap rescan (±5 dtk, preseden ui_font_poll), tanpa
// syscall/IPC baru dan tanpa restart desktop.
#ifndef KYUZEN_DESKTOP_IMPL_WALLPAPER_HPP
#define KYUZEN_DESKTOP_IMPL_WALLPAPER_HPP

#include <stdint.h>
#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/geometry.hpp>
#include "app_icons.hpp"
#include "theme.hpp"

namespace desktop_impl {

using kyuzen::desktop::Canvas;
using kyuzen::desktop::Point;
using kyuzen::desktop::Rect;

class Wallpaper {
public:
    Wallpaper();

    // Baca konfigurasi + decode + skala ke layar w×h. true = foto siap
    // (draw_bg menggambarnya); false = draw_bg memakai gradasi prosedural.
    // Aman dipanggil ulang (buffer lama dibebaskan). Buffer layar (±8 MB
    // @1080p) dialokasikan oleh allocator standar (malloc) — heap libc port
    // tumbuh on-demand sejak Phase 9.5, jadi tidak perlu lagi jalur sys_alloc
    // khusus seperti pada workaround pasca-insiden BSOD INT 6.
    // Gagal muat = wallpaper lama dipertahankan (objek segar = gradasi).
    bool load(int w, int h);
    // Live reload: true = pilihan manifest berubah DAN gambar baru berhasil
    // dimuat (pemanggil: Damage::Full). false = tak berubah / gagal (wallpaper
    // lama tetap, tanpa efek samping). Dipanggil tiap rescan on_poll.
    bool poll(int w, int h);
    bool has_image() const { return px_ != 0; }
    // Pilihan manifest yang sedang tampil ("" = belum pernah load sukses).
    const char* selected() const { return sel_; }

    // Latar untuk `region` (koordinat layar): foto bila ada, jika tidak
    // gradasi prosedural (h_tb = tinggi area bawah yang dikecualikan,
    // yaitu taskbar). Dipakai render Full (region = seluruh layar) maupun
    // Partial (region = area yang perlu dipulihkan).
    void draw_bg(Canvas& canvas, Rect region, int h_tb) const;

    // Foto ke sink (Canvas di app; sink palsu di host test) dibatasi region,
    // dipotong ke ukuran layar. RLE per baris: satu fill_rect per rentang
    // warna identik.
    template <typename Sink>
    void draw_photo_into(Sink& sink, Rect region) const {
        if (!px_ || w_ <= 0 || h_ <= 0) return;
        int x0 = region.x < 0 ? 0 : region.x;
        int y0 = region.y < 0 ? 0 : region.y;
        int x1 = region.x + region.width;
        if (x1 > w_) x1 = w_;
        int y1 = region.y + region.height;
        if (y1 > h_) y1 = h_;
        if (x1 <= x0 || y1 <= y0) return;
        for (int y = y0; y < y1; y++) {
            const uint32_t* row = px_ + y * w_;
            int x = x0;
            while (x < x1) {
                uint32_t v = row[x];
                int n = 1;
                while (x + n < x1 && row[x + n] == v) n++;
                Rect r;
                r.x = x;
                r.y = y;
                r.width = n;
                r.height = 1;
                sink.fill_rect(r, px_color(v));
                x += n;
            }
        }
    }

    // Murni (host-test): indeks nama di WALL_BUILTINS, atau -1.
    static int pick_builtin(const char* name);
    // Murni: "/<nama>" ke out (akar FS). Selalu NUL-terminated.
    static void config_path(char* out, int cap, const char* name);

private:
    void draw_gradient(Canvas& canvas, Rect region, int h_tb) const;

    // Muat `sel` (nama builtin manifest) + fallback bawaan ke buffer BARU;
    // swap ke px_ hanya bila sukses (gagal = lama utuh). true = swap terjadi.
    bool loadSelection(const char* sel, int w, int h);
    // Baca kunci "wallpaper=" manifest ke out (boleh "", selalu NUL).
    static void read_selection(char* out, int cap);

    uint32_t* px_;
    int w_;
    int h_;
    char sel_[32];  // pilihan manifest yang sedang tampil ("")
};

}  // namespace desktop_impl

#endif
