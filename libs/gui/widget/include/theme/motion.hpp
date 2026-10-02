// libs/widget/include/theme/motion.hpp — token durasi + easing.
//
// ATURAN KYUZENOS (kenapa file ini kecil):
// Toolkit ini merender perangkat lunak ke canvas, lalu meng-upload HANYA
// damage rect. Setiap frame animasi = satu render + satu flush. Karena itu
// animasi adalah ANGGARAN, bukan hiasan:
//
//   * hanya transisi yang MENJELASKAN sesuatu (permukaan muncul/hilang),
//   * durasinya di bawah ambang "terasa lambat" (< 200 ms),
//   * jumlah frame dibatasi token, bukan "selama proses berjalan",
//   * hover/press TIDAK dianimasikan: keduanya harus terasa instan dan
//     keduanya mengubah rect kecil — menambahkan frame di sana hanya
//     menambah render tanpa menambah kejelasan.
//
// Semua perhitungan integer (app dibangun -mno-sse -msoft-float).
#ifndef KWIDGET_THEME_MOTION_HPP
#define KWIDGET_THEME_MOTION_HPP

#include "runtime/platform.hpp"

namespace ui {

// ------------------------------------------------------------
// Durasi. Nama = maksud, bukan angka.
//   INSTANT — tidak ada animasi (perubahan harus terasa langsung)
//   FAST    — umpan balik permukaan kecil (menu, tooltip, dropdown)
//   NORMAL  — permukaan mengambang (popover, panel)
//   SLOW    — perpindahan besar (dialog, halaman)
// ------------------------------------------------------------
namespace duration {
constexpr int INSTANT = 0;
constexpr int FAST    = 90;
constexpr int NORMAL  = 130;
constexpr int SLOW    = 180;
}  // namespace duration

// Kurva easing. Dua saja: keluar (decelerate) untuk sesuatu yang MUNCUL,
// masuk (accelerate) untuk sesuatu yang HILANG. Kurva tambahan tidak
// menambah kejelasan, hanya menambah kode.
enum Easing {
    EASE_OUT = 0,   // muncul: cepat di awal, melambat di akhir
    EASE_IN  = 1    // hilang: melambat di awal, cepat di akhir
};

// ------------------------------------------------------------
// Progres 0..255 dari waktu. Mengembalikan 255 (selesai) untuk durasi 0
// sehingga jalur "tanpa animasi" tidak butuh cabang khusus di pemanggil.
// ------------------------------------------------------------
static inline int motion_progress(uint64_t now_ms, uint64_t start_ms, int dur_ms) {
    if (dur_ms <= duration::INSTANT) return 255;
    if (now_ms <= start_ms) return 0;
    uint64_t el = now_ms - start_ms;
    if (el >= (uint64_t)dur_ms) return 255;
    return (int)(el * 255u / (uint64_t)dur_ms);
}

// Easing integer (tanpa float). EASE_OUT: 1-(1-t)^2 ; EASE_IN: t^2.
static inline int motion_ease(int t, Easing e) {
    if (t <= 0) return 0;
    if (t >= 255) return 255;
    if (e == EASE_OUT) {
        int inv = 255 - t;
        return 255 - (inv * inv) / 255;
    }
    return (t * t) / 255;
}

// Progres ter-easing langsung dari waktu — jalur yang dipakai widget.
static inline int motion_value(uint64_t now_ms, uint64_t start_ms, int dur_ms,
                               Easing e) {
    return motion_ease(motion_progress(now_ms, start_ms, dur_ms), e);
}

// Batas jumlah frame sebuah transisi (anggaran render). Dipakai Window untuk
// berhenti mengabari animasi setelah token ini, apa pun keadaan jam — jam
// yang mundur/tidak maju tidak boleh membuat loop merender selamanya.
static inline int motion_max_frames(int dur_ms) {
    // ~16 ms per frame pada loop 60 Hz, plus 2 frame toleransi.
    if (dur_ms <= duration::INSTANT) return 0;
    return dur_ms / 16 + 2;
}

} // namespace ui

#endif // KWIDGET_THEME_MOTION_HPP
