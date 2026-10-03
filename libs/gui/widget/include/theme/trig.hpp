// libs/gui/widget/include/theme/trig.hpp — trigonometri integer (Q15).
//
// KENAPA ADA: jam analog harus menempatkan ujung jarum pada lingkaran, dan
// toolkit ini TIDAK BOLEH memakai float (UI-13.6: seluruh target mewarisi
// KYUZEN_FREESTANDING_BASE dengan -mno-sse -msoft-float, dan kernel tidak
// pernah menyetel CR4.OSFXSR sehingga instruksi SSE fault). Menulis
// `sin(angle)` atau bahkan `cosf()` bukan pilihan.
//
// KENAPA DI theme/ DAN BUKAN DI WIDGET JAM: ini matematika geometri, bukan
// logika jam. CircularProgress memakainya juga, dan widget lain akan
// memakainya nanti. Satu tabel, satu interpolator (UI-13.10).
//
// KETELITIAN: tabel 65 titik per kuadran + interpolasi linear. Kesalahan
// maksimum 2.68 unit Q15 = 0.012 px pada radius 150 px — jauh di bawah
// resolusi layar, dan sudah diverifikasi di tests/host/unit/libui_visual_qa.
#ifndef KWIDGET_THEME_TRIG_HPP
#define KWIDGET_THEME_TRIG_HPP

#include <stdint.h>

namespace ui {

// ------------------------------------------------------------
// Sudut: 1/65536 putaran (fixed-point, bukan derajat/radian).
// Satu putaran penuh = 65536. Dipakai karena pembagian 2/4/8/12/60 bagian
// menjadi pembagian integer yang eksak, tanpa pembulatan.
//
// KONVENSI (penting, mudah salah): sudut 0 = sumbu +X = ARAH JAM 3, dan sudut
// bertambah SEARAH JARUM JAM karena sumbu Y layar mengarah ke bawah. Jadi:
//   0      -> jam 3 (kanan)
//   16384  -> jam 6 (bawah)
//   32768  -> jam 9 (kiri)
//   49152  -> jam 12 (atas)   = ANGLE_12
// Gunakan ANGLE_12 / clock_angle() untuk muka jam, jangan mengasumsikan
// sudut 0 sudah berarti jam 12.
// ------------------------------------------------------------
constexpr int32_t ANGLE_FULL = 65536;
constexpr int32_t ANGLE_HALF = 32768;    // 180 derajat
constexpr int32_t ANGLE_QUARTER = 16384; // 90 derajat
constexpr int32_t ANGLE_12 = ANGLE_FULL - ANGLE_QUARTER;   // 49152 = jam 12

// Bungkus sudut ke [0, 65536) — sudut negatif (mis. jarum jam yang diputar
// mundur) tetap benar.
static inline int32_t angle_norm(int32_t a) {
    a &= (ANGLE_FULL - 1);   // ANGLE_FULL adalah pangkat dua
    return a;
}

// Tabel sin untuk 0..90 derajat dalam 65 langkah, Q15 (32767 = 1.0).
// Monoton naik; kSinQ15[0] = 0, kSinQ15[64] = 32767.
static const int16_t kSinQ15[65] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,
};

// sin(angle) dalam Q15. Hasil di [-32767, 32767].
static inline int32_t sin_q15(int32_t angle) {
    int32_t a = angle_norm(angle);
    int32_t quad = a >> 14;          // 0..3
    int32_t off = a & (ANGLE_QUARTER - 1);
    // Pantulkan ke kuadran pertama. Setelah ini off ada di [0, 16384]:
    // batas ATAS (16384) bisa tercapai tepat pada 90/270 derajat.
    if (quad == 1 || quad == 3) off = ANGLE_QUARTER - off;
    int32_t idx = off >> 8;          // 0..64
    int32_t frac = off & 0xFF;       // 0..255
    int32_t v;
    if (idx >= 64) {
        // Tepat 90 derajat: interpolasi akan membaca kSinQ15[65] yang DI LUAR
        // tabel (bug yang membuat sin(90 derajat) mengembalikan sampah dan
        // jarum jam menunjuk arah acak). Nilainya sudah pasti 1.0.
        v = kSinQ15[64];
    } else {
        v = kSinQ15[idx] + ((kSinQ15[idx + 1] - kSinQ15[idx]) * frac) / 256;
    }
    if (quad >= 2) v = -v;
    return v;
}

// cos(angle) dalam Q15 = sin(angle + 90 derajat).
static inline int32_t cos_q15(int32_t angle) {
    return sin_q15(angle + ANGLE_QUARTER);
}

// Titik pada lingkaran: (cx,cy) + r * (cos,sin).
// Pembulatan ke piksel terdekat (bukan truncate) supaya jarum tidak bias ke
// arah pusat; tanpa ini semua ujung jarum meleset 0.5 px ke dalam.
static inline void point_on_circle(int cx, int cy, int r, int32_t angle,
                                   int* out_x, int* out_y) {
    int32_t c = cos_q15(angle);
    int32_t s = sin_q15(angle);
    int32_t dx = ((int64_t)r * c + (c >= 0 ? 16384 : -16384)) / 32768;
    int32_t dy = ((int64_t)r * s + (s >= 0 ? 16384 : -16384)) / 32768;
    if (out_x) *out_x = cx + (int)dx;
    if (out_y) *out_y = cy + (int)dy;
}

// Sudut dari pecahan putaran: 1 putaran = 65536.
// Dipakai jam: (detik * 65536 / 60) — pembulatan ke bawah disengaja karena
// jarum yang sedikit tertinggal lebih baik daripada yang mendahului waktu.
static inline int32_t angle_from_parts(int64_t part, int64_t total) {
    if (total <= 0) return 0;
    return (int32_t)((part * ANGLE_FULL) / total);
}

// Sudut JARUM untuk `part` dari `total` bagian, diukur dari jam 12.
// Pembungkus tipis di atas angle_from_parts supaya pemanggil tidak perlu tahu
// bahwa konvensi trigonometri menaruh 0 di jam 3.
static inline int32_t clock_angle(int64_t part, int64_t total) {
    if (total <= 0) return ANGLE_12;
    // +ANGLE_12 menggeser titik awal dari jam 3 ke jam 12.
    int64_t a = ANGLE_12 + (part * ANGLE_FULL) / total;
    return (int32_t)(a & (ANGLE_FULL - 1));
}

} // namespace ui

#endif // KWIDGET_THEME_TRIG_HPP
