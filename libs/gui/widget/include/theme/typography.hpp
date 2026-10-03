// libs/widget/include/theme/typography.hpp — peran tipografi semantik.
//
// KENYATAAN RENDERER (baca dulu sebelum mengubah angka):
// Toolkit menggambar teks lewat `gui_draw_text`/`gui_draw_char` = font bitmap
// 8×16 dengan advance TETAP 8px. FreeType TIDAK di-link ke toolkit (itu
// keputusan arsitektur yang dipertahankan: `primitives/fttext.hpp` adalah
// jembatan callback untuk app yang mau tipografi nyata).
//
// Karena itu peran di bawah membawa DUA kelompok field:
//
//   * field "bitmap_*" — yang BENAR-BENAR dipakai jalur bitmap toolkit:
//     tinggi baris, tracking (tambahan advance per karakter), tone, dan
//     apakah teks digambar huruf besar semua.
//   * field "ft_*" — deklarasi NIAT (ukuran px + bobot) untuk app ber-FT
//     (Settings/desktop/fontdemo). Toolkit tidak menerapkannya sendiri, tapi
//     menyediakannya supaya app dan toolkit memakai satu sumber kebenaran
//     alih-alih angka yang tersebar di tiap app.
//
// Hierarki tetap bisa dibangun di jalur bitmap lewat: tinggi baris, tracking,
// tone, dan huruf besar — bukan dengan menambah warna atau kotak.
#ifndef KWIDGET_THEME_TYPOGRAPHY_HPP
#define KWIDGET_THEME_TYPOGRAPHY_HPP

#include "runtime/platform.hpp"
#include "theme/colors.hpp"

namespace ui {

// ------------------------------------------------------------
// Metrik sel bitmap. Satu-satunya sumber angka glyph toolkit.
// ------------------------------------------------------------
namespace glyph {
constexpr int ADVANCE = 8;    // lebar sel glyph (bitmap 8×16)
constexpr int HEIGHT  = 16;   // tinggi glyph yang tergambar
}  // namespace glyph

// Bobot — hanya bermakna di jalur FreeType (bitmap tidak punya bobot).
enum FontWeight {
    WEIGHT_REGULAR = 400,
    WEIGHT_MEDIUM  = 500,
    WEIGHT_SEMIBOLD = 600
};

// ------------------------------------------------------------
// Peran tipografi. `bitmap_line_h` = jarak antar baris (bukan tinggi glyph),
// jadi teks multi-baris bernapas; `bitmap_tracking` = px tambahan per
// karakter (0 = rapat, 1 = terbaca sebagai judul).
// ------------------------------------------------------------
struct TypeRole {
    int  ft_size_px;        // ukuran yang diminta di jalur FreeType
    int  ft_weight;         // FontWeight
    int  bitmap_line_h;     // jarak baris di jalur bitmap
    int  bitmap_tracking;   // tambahan advance per karakter
    bool bitmap_upper;      // gambar sebagai HURUF BESAR (hierarki label kecil)
    TextTone tone;          // tone default peran ini
};

// Satu tabel, satu tempat. Kalau sebuah peran tidak dipakai, hapus barisnya —
// jangan biarkan peran mati menumpuk.
struct Typography {
    TypeRole display;         // angka/hero yang memang butuh besar
    TypeRole title;           // judul halaman / dialog
    TypeRole section;         // judul bagian dalam halaman
    TypeRole body;            // isi teks
    TypeRole body_emphasis;   // isi yang ditekankan (baris terpilih/aktif)
    TypeRole label;           // label kontrol, tombol, baris menu
    TypeRole caption;         // keterangan kecil (hint, meta, status)
    TypeRole mono;            // teks monospace (editor, terminal, angka)

    Typography() {
        display       = make(28, WEIGHT_SEMIBOLD, 32, 1, false, TONE_PRIMARY);
        title         = make(20, WEIGHT_SEMIBOLD, 24, 1, false, TONE_PRIMARY);
        section       = make(14, WEIGHT_SEMIBOLD, 20, 0, true,  TONE_SECONDARY);
        body          = make(14, WEIGHT_REGULAR, 20, 0, false, TONE_PRIMARY);
        body_emphasis = make(14, WEIGHT_MEDIUM, 20, 0, false, TONE_PRIMARY);
        label         = make(14, WEIGHT_MEDIUM, 18, 0, false, TONE_PRIMARY);
        caption       = make(12, WEIGHT_REGULAR, 16, 0, false, TONE_SECONDARY);
        mono          = make(13, WEIGHT_REGULAR, 16, 0, false, TONE_PRIMARY);
    }

    static TypeRole make(int sz, int w, int line_h, int track, bool up,
                         TextTone t) {
        TypeRole r;
        r.ft_size_px = sz;
        r.ft_weight = w;
        r.bitmap_line_h = line_h;
        r.bitmap_tracking = track;
        r.bitmap_upper = up;
        r.tone = t;
        return r;
    }
};

// ------------------------------------------------------------
// Pengukuran teks jalur bitmap — SATU tempat, dipakai semua widget.
// Dulu tiap widget menulis `_ui_strlen(t) * 8` sendiri; dengan tracking
// rumusnya tidak lagi sepele, jadi dihitung sekali di sini.
//
// CATATAN: fungsi ini MENGUKUR JALUR BITMAP. Kalau aplikasi memasang text
// provider (font nyata), pengukuran HARUS lewat core/text_provider.hpp —
// `text_measure_role()` di sana yang memilih jalur dengan benar. Fungsi ini
// tetap ada karena dipakai sebagai nilai default dan oleh test.
// ------------------------------------------------------------
static inline int text_width(const char* s, const TypeRole& r) {
    if (!s) return 0;
    int n = 0;
    while (s[n]) n++;
    return n * (glyph::ADVANCE + r.bitmap_tracking);
}

// Tinggi blok teks n baris pada peran r.
static inline int text_block_height(int lines, const TypeRole& r) {
    if (lines < 1) lines = 1;
    return (lines - 1) * r.bitmap_line_h + glyph::HEIGHT;
}

// Offset vertikal agar glyph 16px tampak berada di tengah kotak setinggi `box`.
// Selalu >= 0 supaya tidak pernah tergambar di luar bounds (damage tracking
// mengandalkan ini).
//
// CATATAN: ini mengukur glyph BITMAP (16px). Widget yang harus mengikuti tinggi
// baris font nyata memakai `text_center_offset()` dari core/text_provider.hpp;
// Painter::text() sudah menempatkan baseline dengan benar untuk kedua jalur,
// jadi nilai ini tetap aman dipakai sebagai offset atas.
static inline int text_vcenter(int box_h) {
    int o = (box_h - glyph::HEIGHT) / 2;
    return o > 0 ? o : 0;
}

} // namespace ui

#endif // KWIDGET_THEME_TYPOGRAPHY_HPP
