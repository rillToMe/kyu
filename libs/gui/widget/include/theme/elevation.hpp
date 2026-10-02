// libs/widget/include/theme/elevation.hpp — model kedalaman (level 0..4).
//
// Aturan KyuzenOS: kedalaman adalah INFORMASI, bukan dekorasi.
//   * Sebuah permukaan naik satu level HANYA kalau ia memang mengambang di
//     atas konten lain (popup, dialog, tooltip).
//   * Konten di dalam halaman (kartu, baris daftar, tombol) TIDAK memakai
//     bayangan sama sekali. Pemisahannya lewat warna permukaan + border 1px.
//   * Tidak pernah menumpuk bayangan + gradient + glow pada satu elemen.
//
// Karena itu tabel di bawah hanya punya 3 permukaan yang boleh berbayang, dan
// level 0..2 punya bayangan nol secara eksplisit — supaya "kenapa tombol tidak
// punya shadow" terjawab di kode, bukan di kepala orang.
#ifndef KWIDGET_THEME_ELEVATION_HPP
#define KWIDGET_THEME_ELEVATION_HPP

#include "runtime/platform.hpp"

namespace ui {

// Level kedalaman. Dipakai widget saat memilih perlakuan permukaan.
enum Elevation {
    ELEV_BASE = 0,      // latar halaman (window bg)
    ELEV_SURFACE = 1,   // permukaan diam (panel, editor, baris daftar)
    ELEV_RAISED = 2,    // permukaan terangkat (tombol, input, tab aktif)
    ELEV_POPUP = 3,     // mengambang di atas konten (menu, dropdown, tooltip)
    ELEV_DIALOG = 4     // modal (dialog, prompt)
};

// ------------------------------------------------------------
// Deskriptor bayangan. `rings` = jumlah cincin alpha; 0 = tanpa bayangan.
// Nilai dipilih supaya kedalaman terbaca di layar gelap DAN terang tanpa
// menjadi noda abu-abu: cincin pertama paling kuat, meluruh cepat.
// ------------------------------------------------------------
struct ShadowSpec {
    int rings;      // 0 = tidak ada bayangan
    int offset_y;   // geser ke bawah (cahaya dari atas)
    int spread;     // tambahan tiap cincin
    int alpha;      // alpha cincin pertama (0..255), meluruh per cincin
    int falloff;    // pengurangan alpha per cincin
};

// Satu tabel kebijakan bayangan per level. Indeks = Elevation.
static inline ShadowSpec elevation_shadow(int level) {
    ShadowSpec s;
    switch (level) {
    case ELEV_POPUP:
        // Menu/tooltip: mengambang dekat konten. Angka ini mempertahankan
        // bayangan popup yang sudah ada (4 cincin, alpha 52→12) supaya menu
        // tidak berubah tampilannya saat token ini diperkenalkan.
        s.rings = 4; s.offset_y = 3; s.spread = 1; s.alpha = 52; s.falloff = 13;
        break;
    case ELEV_DIALOG:
        // Modal: paling jauh dari konten → cincin lebih banyak dan lebih
        // lebar, supaya jarak "di atas segalanya" terbaca tanpa glow.
        s.rings = 6; s.offset_y = 5; s.spread = 1; s.alpha = 56; s.falloff = 9;
        break;
    default:
        // BASE/SURFACE/RAISED: sengaja NOL. Konten halaman tidak berbayang.
        s.rings = 0; s.offset_y = 0; s.spread = 0; s.alpha = 0; s.falloff = 0;
        break;
    }
    return s;
}

// Jarak maksimum yang ditempuh bayangan keluar dari bounds widget (dipakai
// damage tracking supaya area bayangan ikut diminta digambar ulang).
static inline int elevation_shadow_margin(int level) {
    ShadowSpec s = elevation_shadow(level);
    return s.rings ? (s.rings * s.spread + (s.offset_y > 0 ? s.offset_y : 0)) : 0;
}

// ------------------------------------------------------------
// Aturan permukaan: level → token warna mana yang dipakai.
// Satu fungsi supaya widget tidak memilih warna permukaan sendiri-sendiri
// (sumber utama inkonsistensi "kadang surface, kadang surface_elevated").
// ------------------------------------------------------------
// (Didefinisikan di core/theme.hpp karena butuh struct Theme yang lengkap.)

} // namespace ui

#endif // KWIDGET_THEME_ELEVATION_HPP
