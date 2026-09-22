// Kyuzen Desktop — preview hover slot taskbar (kartu STATIS).
//
// Batasan arsitektur (terdokumentasi, bukan bug): WindowInfo framework tidak
// membawa pixel/geometri window, sehingga thumbnail live mustahil TANPA
// mengubah framework/kompositor — di luar scope Phase 9. Kartu menampilkan
// data yang tersedia: ikon app (resolusi terpusat) + judul window + status
// fokus + hint aksi. Tanpa animasi, drag, grouping, cache thumbnail.
//
// Tampil saat kursor di atas slot ikon; hilang saat kursor pergi/diklik.
// Kartu digambar ke canvas window (opaque, satu fill_rect + ikon RLE), jadi
// saat kartu hilang/pindah area bekasnya harus dipulihkan dari wallpaper oleh
// pemanggil (Shell) — `drawn_rect()` memberi kotak yang terakhir digambar.
#ifndef KYUZEN_DESKTOP_IMPL_APP_PREVIEW_HPP
#define KYUZEN_DESKTOP_IMPL_APP_PREVIEW_HPP

#include <stdint.h>
#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/geometry.hpp>
#include <kyuzen/desktop/window_manager.hpp>
#include "app_icons.hpp"
#include "taskbar.hpp"

namespace desktop_impl {

using kyuzen::desktop::Canvas;
using kyuzen::desktop::Point;
using kyuzen::desktop::Rect;
using kyuzen::desktop::WindowInfo;

class AppPreview {
public:
    AppPreview();

    // Tampilkan kartu untuk window w (ikon via app bila cocok, else default).
    // Anchor = slot taskbar (kartu di atasnya, dijepit ke layar).
    void show(const WindowInfo& w, const AppEntry* app, Rect anchor, int wscr,
              int hscr);
    void hide();
    bool visible() const { return on_; }

    // Window yang diwakili (untuk klik-aktivasi).
    uint32_t window_id() const { return win_id_; }
    // Kartu di titik p? (klik di dalam = aktivasi + tutup.)
    bool hit(Point p) const;

    // Geometri kartu (murni, host-test): dari anchor + ukuran layar.
    static Rect card_rect(Rect anchor, int wscr, int hscr);

    // Kotak yang terakhir digambar kartu (0 bila belum/tidak ada). Dipakai
    // render Partial untuk memulihkan wallpaper di bekas kartu.
    Rect drawn_rect() const { return drawn_; }

    // Kartu (latar + ikon + teks) ke canvas window.
    void draw(Canvas& canvas, const IconCache& icons);

private:
    bool on_;
    uint32_t win_id_;
    char title_[32];
    bool focused_;
    const AppEntry* app_;
    Rect card_;
    Rect icon_;
    Rect drawn_;  // kartu terakhir yang benar-benar digambar (untuk restore)
};

}  // namespace desktop_impl

#endif
