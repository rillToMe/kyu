// libs/widget/include/theme/icons.hpp — sistem ikon KyuzenOS.
//
// MASALAH: sebelumnya ikon digambar ad-hoc — `+`/`-` TreeView, segitiga piksel
// ComboBox, kotak centang Menu, dan app menggambar glyph sendiri. Tidak ada
// satu bahasa visual, dan tidak ada cara menyebut ikon secara semantik.
//
// SISTEM INI:
//   * SATU gaya: goresan (stroke) 1px, sudut persegi, tanpa fill, tanpa
//     lengkung bebas — konsisten dengan UI yang tipografi-first.
//   * SATU grid: semua ikon digambar di dalam kotak `size × size` dengan
//     padding optik internal, jadi ketebalan garis terasa sama.
//   * SEMANTIK, bukan bentuk: pemanggil menulis `Icon::ChevronDown`, bukan
//     menggambar segitiga. Bentuknya bisa diperbaiki di satu tempat.
//   * Bukan font ikon dan bukan emoji: digambar dengan primitif Painter
//     (rect per segmen), sehingga tidak ada ketergantungan aset dan tidak ada
//     masalah warna/glyph hilang.
//
// Catatan implementasi: semua ikon dipecah menjadi daftar SEGMEN (garis
// horizontal/vertikal) + titik. Ini membuat gambar ikon menjadi loop sederhana
// tanpa alokasi, tanpa tabel statis global (yang dilarang: ELF loader tidak
// menjalankan .init_array, jadi tidak boleh ada objek global non-trivial —
// array POD `const` di dalam fungsi aman).
#ifndef KWIDGET_THEME_ICONS_HPP
#define KWIDGET_THEME_ICONS_HPP

#include "runtime/platform.hpp"
#include "libui.h"   // UI_ICON_* — kontrak ABI; enum internal HARUS sejajar

namespace ui {

// ------------------------------------------------------------
// Ikon semantik. Tambahkan di sini, JANGAN menggambar bentuk ikon di app.
//
// URUTAN HARUS SAMA PERSIS dengan UI_ICON_* di include/libui.h (dikunci
// static_assert di bawah): nilai dari ABI dipakai langsung sebagai `Icon`,
// jadi pergeseran urutan = ikon yang salah tanpa error kompilasi di sisi app.
// ------------------------------------------------------------
enum Icon {
    ICON_NONE = UI_ICON_NONE,
    // Navigasi
    ICON_CHEVRON_RIGHT = UI_ICON_CHEVRON_RIGHT,
    ICON_CHEVRON_DOWN = UI_ICON_CHEVRON_DOWN,
    ICON_CHEVRON_LEFT = UI_ICON_CHEVRON_LEFT,
    ICON_CHEVRON_UP = UI_ICON_CHEVRON_UP,
    ICON_ARROW_RIGHT = UI_ICON_ARROW_RIGHT,
    ICON_EXPAND = UI_ICON_EXPAND,
    ICON_COLLAPSE = UI_ICON_COLLAPSE,
    // Aksi
    ICON_CLOSE = UI_ICON_CLOSE,
    ICON_CHECK = UI_ICON_CHECK,
    ICON_PLUS = UI_ICON_PLUS,
    ICON_MINUS = UI_ICON_MINUS,
    ICON_SEARCH = UI_ICON_SEARCH,
    ICON_REFRESH = UI_ICON_REFRESH,
    ICON_MORE = UI_ICON_MORE,
    ICON_EDIT = UI_ICON_EDIT,
    ICON_TRASH = UI_ICON_TRASH,
    // Objek / tempat
    ICON_FOLDER = UI_ICON_FOLDER,
    ICON_FILE = UI_ICON_FILE,
    ICON_IMAGE = UI_ICON_IMAGE,
    ICON_HOME = UI_ICON_HOME,
    ICON_STAR = UI_ICON_STAR,
    // Sistem
    ICON_SETTINGS = UI_ICON_SETTINGS,
    ICON_DISPLAY = UI_ICON_DISPLAY,
    ICON_PALETTE = UI_ICON_PALETTE,
    ICON_FONT = UI_ICON_FONT,
    ICON_NETWORK = UI_ICON_NETWORK,
    ICON_POWER = UI_ICON_POWER,
    ICON_INFO = UI_ICON_INFO,
    ICON_WARNING = UI_ICON_WARNING,
    ICON_ERROR = UI_ICON_ERROR,
    ICON_COUNT = UI_ICON_COUNT
};

// Kontrak: enum internal sejajar dengan ABI. Kalau ini gagal, ada ikon yang
// ditambahkan di satu sisi saja.
static_assert((int)ICON_COUNT == (int)UI_ICON_COUNT,
              "theme/icons.hpp dan include/libui.h UI_ICON_* tidak sejajar");
static_assert((int)ICON_ERROR == (int)UI_ICON_ERROR,
              "nilai ikon bergeser terhadap ABI");
static_assert((int)ICON_SETTINGS == (int)UI_ICON_SETTINGS,
              "nilai ikon bergeser terhadap ABI");

// ------------------------------------------------------------
// Spesifikasi geometri ikon: kotak `size`, dan apakah ikon digambar dengan
// goresan 1px (default) atau terisi (dot pada radio/checkmark).
// ------------------------------------------------------------
struct IconStyle {
    int size;       // sisi kotak ikon (px)
    int stroke;     // ketebalan goresan (selalu 1 di toolkit ini)
};

static inline IconStyle icon_style_default() {
    IconStyle s;
    s.size = 16;
    s.stroke = 1;
    return s;
}

// ------------------------------------------------------------
// Ikon digambar sebagai himpunan segmen. `IconSpec` = daftar segmen +
// titik, dalam koordinat 0..15 (grid tetap 16) yang lalu diskalakan ke
// ukuran permintaan. Grid tetap membuat semua ikon optik konsisten.
// ------------------------------------------------------------
struct IconSegment {
    int x0, y0, x1, y1;   // koordinat grid 16 (0..15)
};

// Batas maksimum segmen per ikon (dipakai array lokal, tanpa alokasi).
enum { ICON_MAX_SEGMENTS = 16, ICON_MAX_DOTS = 8 };

struct IconSpec {
    IconSegment segs[ICON_MAX_SEGMENTS];
    int nseg;
    // Titik (dipakai checkmark ujung & dot). `r` = radius dalam grid.
    struct Dot { int x, y, r; };
    Dot dots[ICON_MAX_DOTS];
    int ndot;
};

// Tambah segmen/titik — helper internal.
static inline void icon_seg(IconSpec* s, int x0, int y0, int x1, int y1) {
    if (s->nseg >= ICON_MAX_SEGMENTS) return;
    s->segs[s->nseg].x0 = x0; s->segs[s->nseg].y0 = y0;
    s->segs[s->nseg].x1 = x1; s->segs[s->nseg].y1 = y1;
    s->nseg++;
}
static inline void icon_dot(IconSpec* s, int x, int y, int r) {
    if (s->ndot >= ICON_MAX_DOTS) return;
    s->dots[s->ndot].x = x; s->dots[s->ndot].y = y; s->dots[s->ndot].r = r;
    s->ndot++;
}

// ------------------------------------------------------------
// Bangun geometri ikon pada grid 16×16. Return jumlah elemen (0 = kosong).
// Semua koordinat berada di dalam [2, 13] supaya ada padding optik seragam:
// ikon tidak pernah menyentuh tepi kotaknya.
// ------------------------------------------------------------
static inline IconSpec icon_spec(Icon id) {
    IconSpec s;
    s.nseg = 0;
    s.ndot = 0;
    switch (id) {
    case ICON_CHEVRON_RIGHT:      // >
        icon_seg(&s, 6, 4, 10, 8);
        icon_seg(&s, 10, 8, 6, 12);
        break;
    case ICON_CHEVRON_LEFT:       // <
        icon_seg(&s, 10, 4, 6, 8);
        icon_seg(&s, 6, 8, 10, 12);
        break;
    case ICON_CHEVRON_DOWN:       // v
        icon_seg(&s, 4, 6, 8, 10);
        icon_seg(&s, 8, 10, 12, 6);
        break;
    case ICON_CHEVRON_UP:         // ^
        icon_seg(&s, 4, 10, 8, 6);
        icon_seg(&s, 8, 6, 12, 10);
        break;
    case ICON_ARROW_RIGHT:        // ->
        icon_seg(&s, 3, 8, 12, 8);
        icon_seg(&s, 9, 5, 12, 8);
        icon_seg(&s, 12, 8, 9, 11);
        break;
    case ICON_EXPAND:             // + dalam kotak
        icon_seg(&s, 3, 3, 12, 3);
        icon_seg(&s, 3, 12, 12, 12);
        icon_seg(&s, 3, 3, 3, 12);
        icon_seg(&s, 12, 3, 12, 12);
        icon_seg(&s, 8, 6, 8, 10);
        icon_seg(&s, 6, 8, 10, 8);
        break;
    case ICON_COLLAPSE:           // - dalam kotak
        icon_seg(&s, 3, 3, 12, 3);
        icon_seg(&s, 3, 12, 12, 12);
        icon_seg(&s, 3, 3, 3, 12);
        icon_seg(&s, 12, 3, 12, 12);
        icon_seg(&s, 6, 8, 10, 8);
        break;
    case ICON_CLOSE:              // x
        icon_seg(&s, 5, 5, 11, 11);
        icon_seg(&s, 11, 5, 5, 11);
        break;
    case ICON_CHECK:              // centang
        icon_seg(&s, 4, 8, 7, 11);
        icon_seg(&s, 7, 11, 12, 5);
        break;
    case ICON_PLUS:
        icon_seg(&s, 8, 4, 8, 12);
        icon_seg(&s, 4, 8, 12, 8);
        break;
    case ICON_MINUS:
        icon_seg(&s, 4, 8, 12, 8);
        break;
    case ICON_SEARCH:             // lingkaran + gagang
        icon_seg(&s, 6, 3, 9, 3);
        icon_seg(&s, 6, 12, 9, 12);
        icon_seg(&s, 3, 6, 3, 9);
        icon_seg(&s, 12, 6, 12, 9);
        icon_seg(&s, 4, 4, 5, 4);
        icon_seg(&s, 4, 11, 5, 11);
        icon_seg(&s, 10, 4, 11, 4);
        icon_seg(&s, 10, 11, 11, 11);
        icon_seg(&s, 10, 10, 14, 14);
        break;
    case ICON_REFRESH:            // lingkaran terbuka + panah
        icon_seg(&s, 6, 3, 9, 3);
        icon_seg(&s, 3, 6, 3, 9);
        icon_seg(&s, 6, 12, 9, 12);
        icon_seg(&s, 12, 6, 12, 9);
        icon_seg(&s, 9, 3, 12, 3);
        icon_seg(&s, 12, 3, 12, 6);
        break;
    case ICON_MORE:
        icon_dot(&s, 4, 8, 1);
        icon_dot(&s, 8, 8, 1);
        icon_dot(&s, 12, 8, 1);
        break;
    case ICON_EDIT:               // pensil
        icon_seg(&s, 3, 12, 5, 12);
        icon_seg(&s, 3, 12, 3, 10);
        icon_seg(&s, 3, 10, 10, 3);
        icon_seg(&s, 10, 3, 13, 6);
        icon_seg(&s, 13, 6, 6, 13);
        icon_seg(&s, 6, 13, 3, 12);
        break;
    case ICON_TRASH:
        icon_seg(&s, 4, 4, 12, 4);
        icon_seg(&s, 6, 2, 10, 2);
        icon_seg(&s, 5, 5, 6, 13);
        icon_seg(&s, 11, 5, 10, 13);
        icon_seg(&s, 6, 13, 10, 13);
        break;
    case ICON_FOLDER:
        icon_seg(&s, 2, 5, 7, 5);
        icon_seg(&s, 2, 5, 2, 12);
        icon_seg(&s, 2, 12, 13, 12);
        icon_seg(&s, 13, 5, 13, 12);
        icon_seg(&s, 7, 5, 8, 7);
        icon_seg(&s, 8, 7, 13, 7);
        break;
    case ICON_FILE:
        icon_seg(&s, 4, 2, 4, 13);
        icon_seg(&s, 4, 2, 10, 2);
        icon_seg(&s, 10, 2, 12, 4);
        icon_seg(&s, 12, 4, 12, 13);
        icon_seg(&s, 4, 13, 12, 13);
        icon_seg(&s, 10, 2, 10, 4);
        icon_seg(&s, 10, 4, 12, 4);
        break;
    case ICON_IMAGE:
        icon_seg(&s, 2, 3, 13, 3);
        icon_seg(&s, 2, 12, 13, 12);
        icon_seg(&s, 2, 3, 2, 12);
        icon_seg(&s, 13, 3, 13, 12);
        icon_seg(&s, 3, 11, 6, 7);
        icon_seg(&s, 6, 7, 9, 11);
        icon_seg(&s, 9, 11, 13, 8);
        icon_dot(&s, 5, 6, 1);
        break;
    case ICON_HOME:
        icon_seg(&s, 8, 2, 14, 8);
        icon_seg(&s, 8, 2, 2, 8);
        icon_seg(&s, 4, 7, 4, 13);
        icon_seg(&s, 12, 7, 12, 13);
        icon_seg(&s, 4, 13, 12, 13);
        break;
    case ICON_STAR:
        icon_seg(&s, 8, 2, 8, 5);
        icon_seg(&s, 8, 11, 8, 14);
        icon_seg(&s, 2, 8, 5, 8);
        icon_seg(&s, 11, 8, 14, 8);
        icon_seg(&s, 4, 4, 6, 6);
        icon_seg(&s, 10, 10, 12, 12);
        icon_seg(&s, 12, 4, 10, 6);
        icon_seg(&s, 6, 10, 4, 12);
        icon_dot(&s, 8, 8, 2);
        break;
    case ICON_SETTINGS:           // sliders (bukan gear: lebih mudah dibaca
                                  // pada grid 16 dan lebih tenang)
        icon_seg(&s, 3, 4, 13, 4);
        icon_seg(&s, 3, 8, 13, 8);
        icon_seg(&s, 3, 12, 13, 12);
        icon_dot(&s, 6, 4, 2);
        icon_dot(&s, 10, 8, 2);
        icon_dot(&s, 5, 12, 2);
        break;
    case ICON_DISPLAY:
        icon_seg(&s, 2, 3, 13, 3);
        icon_seg(&s, 2, 10, 13, 10);
        icon_seg(&s, 2, 3, 2, 10);
        icon_seg(&s, 13, 3, 13, 10);
        icon_seg(&s, 8, 10, 8, 13);
        icon_seg(&s, 5, 13, 11, 13);
        break;
    case ICON_PALETTE:
        icon_seg(&s, 4, 5, 11, 5);
        icon_seg(&s, 4, 10, 11, 10);
        icon_seg(&s, 4, 5, 4, 10);
        icon_seg(&s, 11, 5, 11, 10);
        icon_dot(&s, 6, 7, 1);
        icon_dot(&s, 9, 7, 1);
        icon_dot(&s, 6, 9, 1);
        break;
    case ICON_FONT:               // "A" bergaya goresan
        icon_seg(&s, 4, 12, 8, 3);
        icon_seg(&s, 8, 3, 12, 12);
        icon_seg(&s, 6, 9, 10, 9);
        break;
    case ICON_NETWORK:
        icon_seg(&s, 3, 3, 12, 3);
        icon_seg(&s, 3, 12, 12, 12);
        icon_seg(&s, 3, 3, 3, 12);
        icon_seg(&s, 12, 3, 12, 12);
        icon_seg(&s, 3, 8, 12, 8);
        icon_seg(&s, 8, 3, 8, 12);
        break;
    case ICON_POWER:
        icon_seg(&s, 5, 4, 3, 7);
        icon_seg(&s, 3, 7, 3, 10);
        icon_seg(&s, 3, 10, 5, 13);
        icon_seg(&s, 5, 13, 10, 13);
        icon_seg(&s, 10, 13, 12, 10);
        icon_seg(&s, 12, 10, 12, 7);
        icon_seg(&s, 12, 7, 10, 4);
        icon_seg(&s, 10, 4, 8, 3);
        icon_seg(&s, 8, 2, 8, 8);
        break;
    case ICON_INFO:
        icon_seg(&s, 6, 2, 10, 2);
        icon_seg(&s, 6, 13, 10, 13);
        icon_seg(&s, 2, 6, 2, 10);
        icon_seg(&s, 13, 6, 13, 10);
        icon_seg(&s, 3, 3, 4, 3);
        icon_seg(&s, 3, 12, 4, 12);
        icon_seg(&s, 12, 3, 11, 3);
        icon_seg(&s, 12, 12, 11, 12);
        icon_seg(&s, 8, 6, 8, 7);
        icon_seg(&s, 8, 9, 8, 11);
        break;
    case ICON_WARNING:            // segitiga + tanda seru
        icon_seg(&s, 8, 2, 2, 13);
        icon_seg(&s, 8, 2, 13, 13);
        icon_seg(&s, 2, 13, 13, 13);
        icon_seg(&s, 8, 6, 8, 10);
        icon_dot(&s, 8, 12, 0);
        break;
    case ICON_ERROR:              // lingkaran + silang
        icon_seg(&s, 6, 2, 10, 2);
        icon_seg(&s, 6, 13, 10, 13);
        icon_seg(&s, 2, 6, 2, 10);
        icon_seg(&s, 13, 6, 13, 10);
        icon_seg(&s, 3, 3, 4, 3);
        icon_seg(&s, 3, 12, 4, 12);
        icon_seg(&s, 12, 3, 11, 3);
        icon_seg(&s, 12, 12, 11, 12);
        icon_seg(&s, 6, 6, 10, 10);
        icon_seg(&s, 10, 6, 6, 10);
        break;
    case ICON_NONE:
    default:
        break;
    }
    return s;
}

// Nama pendek untuk debugging/log (tanpa alokasi).
static inline const char* icon_name(Icon id) {
    switch (id) {
    case ICON_CHEVRON_RIGHT: return "chevron-right";
    case ICON_CHEVRON_DOWN:  return "chevron-down";
    case ICON_CHEVRON_LEFT:  return "chevron-left";
    case ICON_CHEVRON_UP:    return "chevron-up";
    case ICON_CHECK:         return "check";
    case ICON_CLOSE:         return "close";
    case ICON_SEARCH:        return "search";
    case ICON_SETTINGS:      return "settings";
    default:                 return "icon";
    }
}

} // namespace ui

#endif // KWIDGET_THEME_ICONS_HPP
