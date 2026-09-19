// libs/widget/include/core/theme.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CORE_THEME_HPP
#define KWIDGET_CORE_THEME_HPP

#include "runtime/platform.hpp"

namespace ui {

// ------------------------------------------------------------
// Theme — 6 warna dasar dari ui_theme_t (C ABI) + lapisan turunan, semua
// color_t (libs/color). to_abi() mem-pack kembali format file settings.ui.
//
// ENAM warna ABI di bawah adalah INPUT dari aplikasi. Di atasnya, struct ini
// menurunkan LAPISAN PERMUKAAN (surface layers) gaya Modern Dark:
//   editor   — area teks utama (paling gelap)
//   chrome   — menu bar / status bar (sedikit lebih terang dari editor)
//   panel    — isi modal dialog
//   btnfill  — isian tombol di dalam dialog
//   divider  — garis pemisah 1px antar layer
//   mborder  — border modal (aksen halus)
//   acc_text — teks aksen (shortcut "Ctrl+S" di dropdown, label About)
//   caret    — kursor editor (kontras, jelas)
// Aplikasi cukup set 6 warna dasar; turunannya dihitung sekali per set_theme.
// ------------------------------------------------------------
struct Theme {
    color_t bg, fg, accent, button_bg, button_fg, button_hover;
    color_t editor, chrome, panel, btnfill, divider, mborder, acc_text, caret;
    Theme() : bg(COLOR_RGB(0x1A, 0x1A, 0x2E)), fg(COLOR_RGB(0xE0, 0xE0, 0xE0)),
              accent(COLOR_RGB(0xE9, 0x45, 0x60)), button_bg(COLOR_RGB(0x0F, 0x34, 0x60)),
              button_fg(COLOR_WHITE), button_hover(COLOR_RGB(0x2A, 0x4A, 0x7E)) {
        derive();
    }
    void set(const ui_theme_t* t) {
        // ABI app = color_t; alpha dipaksa opaque. Wajib: warna tema dipakai
        // sebagai dst/latar pencampuran, dan color_blend_alpha membaca
        // dst.a == 0 sebagai "kanvas kosong" sehingga gradien tombol
        // (@rrect_grad) akan rata dengan warna bawahnya.
        bg = color_opaque(t->bg);
        fg = color_opaque(t->fg);
        accent = color_opaque(t->accent);
        button_bg = color_opaque(t->button_bg);
        button_fg = color_opaque(t->button_fg);
        button_hover = color_opaque(t->button_hover);
        derive();
    }
    // 6 warna dasar → struct ABI (color_t), untuk disimpan ke settings.ui.
    void to_abi(ui_theme_t* t) const {
        t->bg = bg;
        t->fg = fg;
        t->accent = accent;
        t->button_bg = button_bg;
        t->button_fg = button_fg;
        t->button_hover = button_hover;
    }
    void derive() {
        // Abjad kecerahan (akhirnya naik): editor < panel < chrome < dialog.
        // Persen shade dihitung agar turunan palet charcoal #1E1E1E jatuh di
        // nilai spesifikasi: panel #252526, chrome #2D2D2D, tombol #3C3C3C,
        // divider #333333, border modal #454545.
        editor = button_bg;                        // "kertas" paling gelap
        panel = COLOR_RGB(0x25, 0x25, 0x26);       // isi modal + popup menu
        chrome = COLOR_RGB(0x2D, 0x2D, 0x2D);      // menubar + status bar
        btnfill = COLOR_RGB(0x3C, 0x3C, 0x3C);     // isian tombol dialog
        divider = COLOR_RGB(0x33, 0x33, 0x33);     // garis pemisah 1px
        mborder = COLOR_RGB(0x45, 0x45, 0x45);     // border modal halus
        // Aksen (konstanta, gaya VS Code — tidak ikut accent app supaya
        // selalu sesuai spesifikasi Modern Dark):
        //   acc_text — shortcut "Ctrl+N" amber #DCDCAA
        //   caret    — kursor editor cyan #00E5FF (kontras di charcoal)
        acc_text = COLOR_RGB(0xDC, 0xDC, 0xAA);
        caret = COLOR_RGB(0x00, 0xE5, 0xFF);
    }
};

// ------------------------------------------------------------
// Warna: color_t + palet libs/color. Campuran lewat color_blend_alpha,
// state tombol lewat color_darken/color_lighten; coverage sudut tetap aa_cov.
// Semua integer — app dibangun -mno-sse -msoft-float.
// ------------------------------------------------------------

// Persen shade lama (aa_shade) → skala 0..255 untuk color_darken/color_lighten.
// Dua rumus tidak identik (aa_shade memakai persen dengan truncate, library
// memakai skala 255 dengan pembulatan); faktor di bawah dipilih supaya hasilnya
// SAMA PERSIS dengan aa_shade untuk palet charcoal tema bawaan
// (#1E1E1E button_bg, #D4D4D4 fg) → tidak ada regresi satu piksel pun di UI.
// Contoh: darken(#1E1E1E, 42) == aa_shade(#1E1E1E, -18) == #191919.
constexpr uint8_t SHADE_5  = 13;    // +5%  (terang)  — lighten(#1E1E1E) = #292929
constexpr uint8_t SHADE_10 = 25;    // ±10% (gradien tombol) — #343434 → #1B1B1B
constexpr uint8_t SHADE_18 = 42;    // -18% (tombol ditekan) — #191919
constexpr uint8_t SHADE_55 = 139;   // -55% (item menu nonaktif) — #606060

} // namespace ui

#endif // KWIDGET_CORE_THEME_HPP
