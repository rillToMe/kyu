// libs/gui/widget/include/core/text_provider.hpp — jembatan tipografi.
//
// MASALAH
// Toolkit menggambar teks dengan font bitmap 8×16 bawaan (`gui_draw_text`), dan
// SEMUA perhitungan tata letaknya memakai asumsi sel tetap 8px. Akibatnya:
//   * pilihan font pengguna (Settings > Fonts, disimpan di `font.ui`) TIDAK
//     berpengaruh pada aplikasi — hanya desktop yang membacanya;
//   * tipografi toolkit tidak bisa berkembang (tidak ada ukuran/bobot nyata).
//
// SOLUSI: SATU titik serah — "text provider".
// Toolkit TIDAK men-link FreeType (keputusan arsitektur yang dipertahankan:
// `primitives/fttext.hpp` tetap jembatan untuk app yang mau menggambar sendiri).
// Yang ditambahkan adalah KONTRAK: kalau sebuah aplikasi memasang provider,
// toolkit memakai provider itu untuk MENGUKUR dan MENGGAMBAR semua teks, dan
// seluruh tata letaknya ikut menyesuaikan. Tanpa provider, toolkit jatuh ke
// bitmap 8×16 seperti sebelumnya — jadi test host dan aplikasi lama tetap
// bekerja tanpa perubahan.
//
// KENAPA GLOBAL (dan bukan parameter per-window)
// Font UI adalah SATU keputusan per proses: pengguna memilih satu font di
// Settings dan mengharapkan SELURUH aplikasi mengikutinya. Menjadikannya
// per-window akan memungkinkan dua aplikasi menampilkan font berbeda untuk
// dokumen yang sama — persis yang tidak diinginkan. Storage-nya satu variabel
// file-scope di `src/core/text.cpp` (satu definisi, ODR), bukan tabel.
//
// KONTRAK PROVIDER
//   * `measure` mengembalikan lebar advance satu baris teks dalam px.
//   * `line_height` = jarak antar baris (>= tinggi glyph).
//   * `ascent` = jarak dari atas baris ke baseline. Dipakai untuk menempatkan
//     teks secara vertikal di dalam kotak kontrol.
//   * `draw` menulis langsung ke canvas (ARGB8888, stride = cw). Return 1 bila
//     berhasil. Toolkit menandai damage-nya sendiri, jadi provider TIDAK perlu
//     memanggil gui_damage_rect().
// Semua fungsi boleh 0 (tidak didukung) — toolkit menanganinya.
#ifndef KWIDGET_CORE_TEXT_PROVIDER_HPP
#define KWIDGET_CORE_TEXT_PROVIDER_HPP

#include "runtime/platform.hpp"
#include "theme/typography.hpp"

namespace ui {

struct TextProvider {
    // Lebar advance teks (UTF-8, n byte). 0 = provider tidak dipakai.
    int (*measure)(void* ud, const char* text, int n);
    // Jarak antar baris dan jarak atas→baseline, dalam px.
    int (*line_height)(void* ud);
    int (*ascent)(void* ud);
    // Gambar teks pada (x, baseline_y). Return 1 bila tergambar.
    void (*draw)(void* ud, uint32_t* canvas, int cw, int ch, int x,
                 int baseline_y, color_t c, const char* text);
    void* ud;
};

// Pasang provider proses (0 = kembali ke bitmap 8×16). Idempoten.
void text_provider_set(const TextProvider* p);
const TextProvider* text_provider_get();

// Helper aman: apakah provider terpasang DAN lengkap untuk menggambar?
bool text_provider_active();

// ------------------------------------------------------------
// Pengukuran statis — dipakai WIDGET (constructor/hit-test) dan Painter.
// Widget lama menghitung `len * 8` sendiri; dengan provider, lebar teks tidak
// lagi konstanta, jadi perhitungan itu harus lewat SATU fungsi. Widget yang
// masih memakai `len * 8` akan salah ukuran begitu font pengguna diganti.
// ------------------------------------------------------------
static inline int text_measure(const char* s) {
    if (!s) return 0;
    int n = 0;
    while (s[n]) n++;
    const TextProvider* tp = text_provider_get();
    if (tp) return tp->measure(tp->ud, s, n);
    return n * glyph::ADVANCE;
}

// Lebar teks pada peran tipografi: bitmap menambah tracking, provider tidak
// (ukuran huruf nyata sudah memberi hierarki tanpa trik tracking).
static inline int text_measure_role(const char* s, const TypeRole& r) {
    if (!s) return 0;
    if (text_provider_active()) return text_measure(s);
    int n = 0;
    while (s[n]) n++;
    return n * (glyph::ADVANCE + r.bitmap_tracking);
}

// Tinggi satu baris teks (jarak antar baris).
static inline int text_line_height() {
    const TextProvider* tp = text_provider_get();
    if (tp) {
        int lh = tp->line_height(tp->ud);
        if (lh > 0) return lh;
    }
    return glyph::HEIGHT;
}

// Jarak dari atas baris ke baseline.
static inline int text_ascent() {
    const TextProvider* tp = text_provider_get();
    if (tp && tp->ascent) {
        int a = tp->ascent(tp->ud);
        if (a > 0) return a;
    }
    return glyph::HEIGHT - 2;   // baseline bitmap 8x16 (descender 2px)
}

// Offset vertikal agar teks tampak di tengah kotak setinggi `box`.
// (Nama berbeda dari `text_vcenter()` di theme/typography.hpp, yang khusus
// jalur bitmap: fungsi ini mengikuti tinggi baris provider.)
static inline int text_center_offset(int box) {
    int o = (box - text_line_height()) / 2;
    return o > 0 ? o : 0;
}

} // namespace ui

#endif // KWIDGET_CORE_TEXT_PROVIDER_HPP
