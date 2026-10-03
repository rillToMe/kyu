// apps/clock/bigtime.hpp — angka waktu ukuran besar (hero) untuk halaman Clock.
//
// KENAPA MEMAKAI FtText + FONT MILIK APLIKASI (bukan label toolkit):
// teks toolkit digambar oleh text provider proses pada SATU ukuran (15 px,
// lihat libs/gui/widget/src/services/uifont.cpp). Ukuran itu benar untuk
// chrome aplikasi — label, tombol, baris daftar — tetapi waktu utama sebuah jam
// harus mendominasi layar, dan itu butuh satu ukuran lagi.
//
// Pola ini SUDAH ADA dan disahkan: `primitives/fttext.hpp` adalah jembatan
// resmi untuk "aplikasi ingin menggambar teksnya sendiri", dan browser memakai
// jalur yang sama untuk kontennya. Toolkit inti sengaja tidak men-link
// FreeType, jadi font kedua ini dimiliki aplikasi.
//
// IDEMPOTEN (UI-8.3): menggambar memakai kz_text_draw_on(), bukan
// kz_text_draw(). Widget memang menggambar ulang area yang sama (ScrollView
// menggambar anaknya dua kali per frame; hover melukis ulang sebagian baris),
// dan kz_text_draw() yang menumpuk akan menebalkan tepi glyph tiap kali.
#ifndef CLOCK_BIGTIME_HPP
#define CLOCK_BIGTIME_HPP

#include <stdint.h>

#include "platform.hpp"

namespace clock_app {

// Ukuran font hero (px). Satu tempat, bukan angka tersebar.
//
// 64 px, bukan 46: pada komposisi compact (lihat ui/xml/clock.xml) waktu harus
// MENDOMINASI halaman, dan pada jendela selebar ~460 px teks 64 px mengambil
// sekitar 40% lebar - cukup untuk jadi fokus tanpa mendorong elemen lain keluar.
constexpr uint32_t HERO_PX = 64;
// Ukuran teks pendukung yang juga digambar sendiri (detik).
constexpr uint32_t SUB_PX = 20;

// Satu font milik aplikasi, dipakai untuk kedua ukuran di atas.
// `load` mengikuti pilihan font pengguna (/font.ui) persis seperti
// ui::uifont_install(), tetapi memuat instance TERPISAH dengan ukuran sendiri
// sehingga mengubah ukuran di sini tidak mempengaruhi widget toolkit.
class BigFont {
public:
    BigFont();
    ~BigFont();

    // Return 1 bila font nyata terpasang, 0 bila gagal (aplikasi tetap jalan
    // dengan fallback bitmap — lihat catatan di draw_*).
    int load();

    bool ready() const { return font_ != 0; }

    // Ukuran satu baris teks pada font hero/pendukung. Selalu > 0.
    void measure_hero(const char* s, int* w, int* h) const;
    void measure_sub(const char* s, int* w, int* h) const;

    // Gambar pada canvas mentah (dipakai callback FtText). `bg` = permukaan di
    // belakang teks; WAJIB diberikan agar komposit idempoten (UI-8.2).
    // Return 0 bila tidak ada font (pemanggil menggambar fallback).
    int draw_hero(uint32_t* canvas, int cw, int ch, int x, int baseline_y,
                  color_t fg, color_t bg, const char* text);
    int draw_sub(uint32_t* canvas, int cw, int ch, int x, int baseline_y,
                 color_t fg, color_t bg, const char* text);

    uint32_t hero_px() const { return HERO_PX; }
    uint32_t sub_px() const { return SUB_PX; }

private:
    kz_font_t* font_;      // 0 = tidak ada (fallback)
    void*      blob_;      // buffer TTF milik aplikasi
};

}  // namespace clock_app

#endif // CLOCK_BIGTIME_HPP
