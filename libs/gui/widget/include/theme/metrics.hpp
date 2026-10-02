// libs/widget/include/theme/metrics.hpp — skala jarak, radius, dan ukuran kontrol.
//
// Tujuan: tidak ada lagi angka ajaib seperti `padding = 13` atau `radius = 7`.
// Setiap widget membaca token di sini. Kalau sebuah nilai tidak ada di sini,
// artinya nilai itu harus DITAMBAHKAN sebagai token (dengan alasan), bukan
// ditulis langsung di widget.
//
// Skala jarak mengikuti deret 4px — cukup rapat untuk layar desktop, cukup
// longgar untuk hirarki. Nilai di luar deret hanya boleh muncul bila ada
// alasan terdokumentasi (mis. tinggi kontrol yang diturunkan dari 16px glyph).
#ifndef KWIDGET_THEME_METRICS_HPP
#define KWIDGET_THEME_METRICS_HPP

#include "runtime/platform.hpp"
#include "theme/typography.hpp"

namespace ui {

// ------------------------------------------------------------
// Skala jarak. SATU deret, dipakai untuk padding, gap, dan inset.
// ------------------------------------------------------------
namespace space {
constexpr int XS  = 4;
constexpr int SM  = 8;
constexpr int MD  = 12;
constexpr int LG  = 16;
constexpr int XL  = 24;
constexpr int XXL = 32;
constexpr int XXXL = 48;
}  // namespace space

// ------------------------------------------------------------
// Radius. Sedikit level, masing-masing punya arti:
//   NONE      — permukaan penuh/tepi keras (bar, tabel, editor)
//   SMALL     — elemen kecil (kotak centang, track)
//   CONTROL   — kontrol interaktif (tombol, input, dropdown)
//   CONTAINER — wadah (kartu bagian, panel, menu, popup)
//   DIALOG    — permukaan modal terbesar
// Pill hanya untuk elemen yang memang berbentuk kapsul (badge), bukan kontrol.
// ------------------------------------------------------------
namespace radius {
constexpr int NONE      = 0;
constexpr int SMALL     = 3;
constexpr int CONTROL   = 5;
constexpr int CONTAINER = 7;
constexpr int DIALOG    = 9;
constexpr int PILL      = 999;   // dijepit setengah tinggi oleh painter
}  // namespace radius

// ------------------------------------------------------------
// Ukuran kontrol. Tinggi diturunkan dari glyph 16px + napas vertikal:
//   SM = 16 + 2*4  = 24   (kontrol rapat di dalam baris/daftar)
//   MD = 16 + 2*6  = 28   (default toolkit; tombol, input, dropdown)
//   LG = 16 + 2*10 = 36   (aksi utama halaman)
// ------------------------------------------------------------
namespace control {
constexpr int H_SM = 24;
constexpr int H_MD = 28;
constexpr int H_LG = 36;
constexpr int PAD_X = 12;      // padding horizontal teks tombol
constexpr int PAD_X_SM = 8;    // varian rapat (toolbar, chip)
}  // namespace control

// ------------------------------------------------------------
// Bar chrome (menubar/toolbar/statusbar). Tingginya bukan turunan glyph saja:
// bar harus terasa sebagai pita, bukan baris teks.
// ------------------------------------------------------------
namespace chrome {
constexpr int MENUBAR_H = 26;
constexpr int TOOLBAR_H = 34;
constexpr int STATUSBAR_H = 24;
constexpr int TAB_STRIP_H = 32;
constexpr int MENU_ROW_H = 24;
constexpr int MENU_SEP_H = 9;
}  // namespace chrome

// ------------------------------------------------------------
// Daftar (list/table/tree/grid). Row 24 = 16 glyph + 2*4 napas.
// ------------------------------------------------------------
namespace list {
constexpr int ROW_H = 24;
constexpr int ROW_H_SM = 20;
constexpr int HEADER_H = 26;
constexpr int INDENT = 16;      // indentasi per level (TreeView)
}  // namespace list

// ------------------------------------------------------------
// Scrollbar. BAR = lebar/tinggi track; THUMB_MIN = panjang minimum thumb
// supaya selalu bisa digenggam mouse.
// (Namespace bernama `scrollbar`, bukan `scroll`: `Scrollable` punya member
// `scroll`, dan member akan menutupi nama namespace di dalam kelasnya.)
// ------------------------------------------------------------
namespace scrollbar {
constexpr int BAR = 8;
constexpr int THUMB_MIN = 24;
constexpr int TRACK_INSET = 2;
}  // namespace scrollbar

// ------------------------------------------------------------
// Fokus. Ketebalan ring fokus dan inset-nya dari tepi kontrol.
// Fokus digambar DI DALAM bounds widget (damage tracking mengandalkan itu).
// ------------------------------------------------------------
namespace focus {
constexpr int RING_W = 1;       // border fokus
constexpr int OUTLINE_W = 2;    // underline fokus (tab, baris daftar)
}  // namespace focus

// ------------------------------------------------------------
// Ikon. Satu ukuran optik standar supaya semua ikon terasa satu keluarga.
// ------------------------------------------------------------
namespace icon {
constexpr int SM = 12;   // di dalam baris teks (checkbox, chevron)
constexpr int MD = 16;   // default: sejajar glyph
constexpr int LG = 20;   // toolbar/rail
constexpr int XL = 24;   // navigasi besar
}  // namespace icon

// ------------------------------------------------------------
// Border. Selalu 1px di toolkit ini (skala piksel); token ada supaya
// kebijakan "berapa banyak border" bisa dibaca di satu tempat.
// ------------------------------------------------------------
namespace border {
constexpr int HAIRLINE = 1;
}  // namespace border

// ------------------------------------------------------------
// Scrollbar/daftar memakai tinggi baris dari tema yang sama; konstanta di
// atas adalah nilai default yang dipakai widget saat tidak ada override.
// ------------------------------------------------------------
struct Metrics {
    // Jarak
    int xs, sm, md, lg, xl, xxl, xxxl;
    // Radius
    int radius_small, radius_control, radius_container, radius_dialog;
    // Kontrol
    int control_h, control_h_sm, control_h_lg, control_pad_x;
    // Chrome
    int menubar_h, toolbar_h, statusbar_h, tab_strip_h;
    int menu_row_h, menu_sep_h;
    // Daftar
    int row_h, row_h_sm, list_header_h, list_indent;
    // Scrollbar
    int scrollbar_w, scrollbar_thumb_min, scrollbar_inset;
    // Fokus & ikon
    int focus_ring, focus_outline;
    int icon_sm, icon_md, icon_lg, icon_xl;
    // Layar (diisi Window dari ukuran window; dipakai layout adaptif)
    int content_pad;        // margin tepi konten halaman

    Metrics()
        : xs(space::XS), sm(space::SM), md(space::MD), lg(space::LG),
          xl(space::XL), xxl(space::XXL), xxxl(space::XXXL),
          radius_small(radius::SMALL), radius_control(radius::CONTROL),
          radius_container(radius::CONTAINER), radius_dialog(radius::DIALOG),
          control_h(control::H_MD), control_h_sm(control::H_SM),
          control_h_lg(control::H_LG), control_pad_x(control::PAD_X),
          menubar_h(chrome::MENUBAR_H), toolbar_h(chrome::TOOLBAR_H),
          statusbar_h(chrome::STATUSBAR_H), tab_strip_h(chrome::TAB_STRIP_H),
          menu_row_h(chrome::MENU_ROW_H), menu_sep_h(chrome::MENU_SEP_H),
          row_h(list::ROW_H), row_h_sm(list::ROW_H_SM),
          list_header_h(list::HEADER_H), list_indent(list::INDENT),
          scrollbar_w(scrollbar::BAR), scrollbar_thumb_min(scrollbar::THUMB_MIN),
          scrollbar_inset(scrollbar::TRACK_INSET),
          focus_ring(focus::RING_W), focus_outline(focus::OUTLINE_W),
          icon_sm(icon::SM), icon_md(icon::MD), icon_lg(icon::LG),
          icon_xl(icon::XL),
          content_pad(space::XL) {}

    // Tinggi baris daftar yang memuat peran teks tertentu (satu tempat).
    int row_height_for(const TypeRole& r) const {
        return text_block_height(1, r) + 2 * space::XS;
    }
};

// ------------------------------------------------------------
// Helper geometri bersama — menghindari rumus pixel-alignment yang ditulis
// ulang di tiap widget.
// ------------------------------------------------------------
// Snap ke grid 2px: teks bitmap 8×16 hanya tajam pada offset tertentu, dan
// border 1px + radius ganjil mudah menghasilkan tepi buram. Snap pada sumbu
// yang tidak dipakai glyph.
static inline int snap(int v) { return v & ~1; }

// Rect dalam batas [0, limit): dipakai untuk clamp popup/tooltip.
static inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace ui

#endif // KWIDGET_THEME_METRICS_HPP
