// Kyuzen Desktop — taskbar Phase 9: slot ikon app + area sistem kanan.
//
// Kiri: satu slot per window (ikon 28px + status fokus/hover; TANPA judul —
// judul pindah ke kartu preview). Kanan: area sistem (jam "HH:MM" +
// tanggal numerik "DD MM YYYY"). Query window via WindowManager framework;
// ikon via Launcher + IconCache terpusat; jam via sys_get_time (UTC).
//
// ZONA WAKTU: sys_get_time melaporkan UTC (drivers/rtc.c tidak lagi
// menambahkan WIB). Taskbar menerjemahkannya ke waktu lokal memakai aturan
// zona dari /time.ui, yang dipasang desktop lewat set_zone(). Tanpa
// set_zone(), zona default = UTC — itu menjaga kelas ini tetap murni dan
// bisa diuji di host tanpa filesystem.
//
// Tidak ada sistem tracking window kedua: status fokus dari WindowInfo,
// hover dari event MouseMove (update_hover), jam dari RTC kernel.
//
// Strip digambar penuh (satu fill_rect opaque) + ikon di atasnya ke canvas
// window desktop. Strategi "lubang alpha-0 untuk base_canvas" Phase 9 awal
// dibuang: window desktop selalu opaque & full-screen, jadi base_canvas tidak
// pernah terlihat (ikon tampak hilang).
#ifndef KYUZEN_DESKTOP_IMPL_TASKBAR_HPP
#define KYUZEN_DESKTOP_IMPL_TASKBAR_HPP

#include <stdint.h>
#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/geometry.hpp>
#include <kyuzen/desktop/window_manager.hpp>
#include "app_icons.hpp"
#include "launcher.hpp"
#include "kztime.h"   // KZ_TZ_MAX (aturan zona untuk jam lokal)

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

    // Query ulang via wm (+ cocokkan ikon via launcher, baca jam RTC).
    // true = tampilan berubah (perlu damage Partial).
    bool poll(WindowManager& wm, const Launcher& launcher);
    int count() const { return nwins_; }  // mentah (termasuk desktop)
    const WindowInfo& entry(int i) const { return wins_[i]; }

    // Slot tampil (window terfilter: bukan desktop + berjudul).
    int slots() const { return nslots_; }
    // Indeks wins_ untuk slot s.
    int slot_window(int s) const { return slot_win_[s]; }
    // App launcher yang judulnya cocok window slot s (boleh null ->
    // ikon default). Kecocokan dihitung saat poll().
    const AppEntry* slot_app(int s) const { return slot_app_[s]; }

    // Geometri slot s di layar w×h (murni, untuk hit + gambar + preview).
    static Rect slot_rect(int s, int w, int h);
    // Posisi ikon 28px dalam slot (murni).
    static Rect slot_icon_rect(int s, int w, int h);
    // Slot di titik p (p sudah di strip taskbar), atau -1.
    int find_slot(Point p, int w, int h) const;

    // Hover dari MouseMove; true = berubah (perlu damage Partial).
    bool update_hover(Point p, int w, int h);
    int hovered() const { return hovered_; }

    // Teks jam (host-test membaca format numerik).
    const char* clock_time() const { return time_; }
    const char* clock_date() const { return date_; }

    // Aturan zona waktu POSIX untuk waktu lokal (mis. "WIB-7"). Desktop
    // memanggil ini sekali saat startup dari /time.ui. String disalin.
    void set_zone(const char* rule);
    // Format jam mengikuti preferensi pengguna (Settings > Date & Time):
    // 1 = 12 jam dengan AM/PM. Default 0 = 24 jam, sama seperti sebelumnya.
    void set_hour12(int on) { hour12_ = on ? 1 : 0; }

    // Gambar strip + ikon + jam/tanggal ke canvas window. w×h = ukuran layar
    // (eksplisit; host test bisa menguji tanpa window). Ikon bergambar
    // (RLE fill_rect) dihitung di nilai balik (diagnostik serial).
    int draw(Canvas& canvas, const IconCache& icons, int w, int h) const;

private:
    WindowInfo wins_[MAX_WINS];
    int nwins_;
    int slot_win_[MAX_WINS];
    const AppEntry* slot_app_[MAX_WINS];
    int nslots_;
    int hovered_;  // slot, atau -1
    // "HH:MM" atau "h:MM AM" (12 jam). Buffer cukup untuk keduanya.
    char time_[12];
    char date_[16];  // "DD MM YYYY"
    char zone_[KZ_TZ_MAX];   // aturan zona lokal ("" = UTC)
    int hour12_;             // 0 = 24 jam, 1 = 12 jam (dari /time.ui)
    void rebuild_slots(const Launcher& launcher, int w);
};

}  // namespace desktop_impl

#endif
