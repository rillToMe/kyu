// libs/widget/include/core/theme.hpp — sistem tema Phase A.
//
// Model:
//   ThemeConfig (ui_theme_config_t: mode × aksen) → palet semantik → widget.
//   Jalur legacy (ui_theme_t 6 warna) tetap didukung: 6 field lama diisi
//   persis seperti dulu + role semantik dipetakan 1:1 dari nilainya, sehingga
//   aplikasi legacy tampil identik piksel-per-piksel.
//
// Semua color_t (libs/color), integer-only (app dibangun -mno-sse -msoft-float).
#ifndef KWIDGET_CORE_THEME_HPP
#define KWIDGET_CORE_THEME_HPP

#include "runtime/platform.hpp"

namespace ui {

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

// ------------------------------------------------------------
// Base aksen (0xRRGGBB). Satu warna per aksen; state (hover/pressed/
// subtle/contrast) DITURUNKAN deterministik di apply_config(), bukan
// di-hardcode per kombinasi mode × aksen.
// ------------------------------------------------------------
static inline color_t theme_accent_base(ui_theme_accent_t a, color_t custom) {
    switch (a) {
    case UI_ACCENT_BLUE:   return color_hex(0x2F81F7);
    case UI_ACCENT_PURPLE: return color_hex(0xA371F7);
    case UI_ACCENT_GREEN:  return color_hex(0x3FB950);
    case UI_ACCENT_ORANGE: return color_hex(0xE8912D);
    case UI_ACCENT_RED:    return color_hex(0xF85149);
    case UI_ACCENT_CUSTOM: {
        color_t c = color_opaque(custom);
        // Hitam pekat = aksen degenerat (hover/pressed tak terlihat);
        // jatuhkan ke netral. Warna custom lain dipakai apa adanya.
        if (c.r == 0 && c.g == 0 && c.b == 0) break;
        return c;
    }
    case UI_ACCENT_NEUTRAL:
    default: break;
    }
    return color_hex(0x8B8B8B);   // NEUTRAL (default)
}

// Campuran opaque: t/255 bobot fg di atas bg. Tinta seleksi/subtle di atas
// permukaan (painter rect opaque — tanpa alpha blending di sini).
static inline color_t theme_mix(color_t fg, color_t bg, uint8_t t) {
    color_t out;
    out.r = (uint8_t)(((uint32_t)fg.r * t + (uint32_t)bg.r * (255u - t)) / 255u);
    out.g = (uint8_t)(((uint32_t)fg.g * t + (uint32_t)bg.g * (255u - t)) / 255u);
    out.b = (uint8_t)(((uint32_t)fg.b * t + (uint32_t)bg.b * (255u - t)) / 255u);
    out.a = 255;
    return out;
}

// ------------------------------------------------------------
// Theme — field legacy (6 + 8, NAMA DAN MAKNA TAK BERUBAH) + peran
// semantik Phase A. Widget membaca peran semantik; field legacy
// dipertahankan untuk ABI ui_theme_t + file settings.ui v1/v0.
//
// Peran semantik dan pemiliknya (Phase A):
//   surface / surface_elevated — tombol, input, track, header, hover baris
//   text / text_secondary / text_disabled — teks (primer, sekunder, redup)
//   border / border_subtle — garis tepi / pemisah halus
//   accent* — aksen + turunannya (slider, progress, scrollbar, tab, fokus)
//   selection — baris terpilih (tinta aksen di atas background)
//   focus — border/caret fokus (== accent)
//   success / warning / danger — diagnostik (dicadangkan Phase B)
// ------------------------------------------------------------
struct Theme {
    // Legacy: 6 warna dasar ui_theme_t + 8 lapisan turunan.
    color_t bg, fg, accent, button_bg, button_fg, button_hover;
    color_t editor, chrome, panel, btnfill, divider, mborder, acc_text, caret;
    // Semantik Phase A.
    color_t surface, surface_elevated;
    color_t text, text_secondary, text_disabled;
    color_t border, border_subtle;
    color_t accent_hover, accent_pressed, accent_subtle, accent_contrast;
    color_t selection, focus;
    color_t success, warning, danger;

    // Default = Dark + Neutral. TIDAK ada lagi ketergantungan biru
    // (0x0F3460/0x2A4A7E) di tema bawaan.
    Theme() {
        static const ui_theme_config_t def = { UI_THEME_DARK, UI_ACCENT_NEUTRAL,
                                               { 0, 0, 0, 255 } };
        apply_config(&def);
    }
    // Jalur legacy: 6 warna eksplisit aplikasi. Field legacy diisi persis
    // seperti dulu (derive_legacy); peran semantik dipetakan 1:1 dari nilai
    // yang sama sehingga widget tampil identik.
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
        derive_legacy();
        // surface layers: nilai lama, peran baru.
        surface = button_bg;
        surface_elevated = button_hover;
        selection = button_bg;
        focus = accent;
        text = fg;
        // Campuran terikat-teks (bukan konstanta) agar tema terang/gelap
        // legacy tetap terbaca.
        text_secondary = theme_mix(fg, bg, 165);
        // Sama persis dengan rumus menu-disabled lama (piksel-identik).
        text_disabled = color_darken(fg, SHADE_55);
        border = mborder;
        border_subtle = divider;
        derive_accent_family(accent, false);
        // Status: belum dipakai widget Phase A (dicadangkan Phase B).
        success = color_hex(0x3FB950);
        warning = color_hex(0xD29922);
        danger = color_hex(0xF85149);
    }
    // Jalur Phase A: mode × aksen → palet semantik penuh; field legacy
    // diisi DARI peran semantik (kompatibel dibaca/ditulis format lama).
    void apply_config(const ui_theme_config_t* c) {
        bool light = c && c->mode == UI_THEME_LIGHT;
        ui_theme_accent_t a = c ? c->accent : UI_ACCENT_NEUTRAL;
        color_t custom = c ? c->custom : color_hex(0x000000);
        color_t abase = theme_accent_base(a, custom);

        if (light) {
            bg = color_hex(0xF5F5F5);
            surface = color_hex(0xFFFFFF);
            surface_elevated = color_hex(0xEAEAEA);
            text = color_hex(0x181818);
            text_secondary = color_hex(0x666666);
            text_disabled = theme_mix(text, bg, 115);
            border = color_hex(0xD6D6D6);
            border_subtle = color_hex(0xE3E3E3);
            success = color_hex(0x1A7F37);
            warning = color_hex(0x9A6700);
            danger = color_hex(0xCF222E);
        } else {
            bg = color_hex(0x111111);
            surface = color_hex(0x181818);
            surface_elevated = color_hex(0x232323);
            text = color_hex(0xF2F2F2);
            text_secondary = color_hex(0xA8A8A8);
            text_disabled = theme_mix(text, bg, 115);
            border = color_hex(0x303030);
            border_subtle = color_hex(0x262626);
            success = color_hex(0x3FB950);
            warning = color_hex(0xD29922);
            danger = color_hex(0xF85149);
        }
        accent = abase;
        derive_accent_family(abase, light);
        focus = accent;
        // Tinta seleksi: aksen di atas background (terbaca di kedua mode,
        // berbeda dari permukaan tombol).
        selection = theme_mix(accent, bg, light ? 51 : 77);

        // Field legacy ← peran semantik (arsitektur lama tetap jalan).
        fg = text;
        button_bg = surface;
        button_fg = text;
        button_hover = surface_elevated;
        editor = surface;
        chrome = surface;
        panel = surface_elevated;
        btnfill = surface_elevated;
        divider = border_subtle;
        mborder = border;
        acc_text = text_secondary;
        caret = accent;
    }
    // Keluarga aksen dari satu base + mode. Dipakai kedua jalur (legacy =
    // gaya dark) agar custom accent konsisten di mana pun dimuat.
    void derive_accent_family(color_t abase, bool light) {
        if (light) {
            accent_hover = color_darken(abase, 22);
            accent_pressed = color_darken(abase, 42);
        } else {
            accent_hover = color_lighten(abase, 32);
            accent_pressed = color_darken(abase, 36);
        }
        accent_subtle = theme_mix(abase, bg, 38);
        accent_contrast = color_get_contrast_text(abase);
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
    // Turunan legacy: konstanta charcoal/amber/cyan. HANYA dipakai jalur
    // set() (tema 6-warna eksplisit) sebagai shim kompatibilitas — aplikasi
    // legacy tampil identik. Jalur config TIDAK menyentuhnya.
    void derive_legacy() {
        editor = button_bg;                        // "kertas" paling gelap
        panel = COLOR_HEX(0x252526);       // isi modal + popup menu
        chrome = COLOR_HEX(0x2D2D2D);      // menubar + status bar
        btnfill = COLOR_HEX(0x3C3C3C);     // isian tombol dialog
        divider = COLOR_HEX(0x333333);     // garis pemisah 1px
        mborder = COLOR_HEX(0x454545);     // border modal halus
        // Aksen (konstanta, gaya VS Code):
        //   acc_text — shortcut "Ctrl+N" amber #DCDCAA
        //   caret    — kursor editor cyan #00E5FF (kontras di charcoal)
        acc_text = COLOR_HEX(0xDCDCAA);
        caret = COLOR_HEX(0x00E5FF);
    }
};

// ------------------------------------------------------------
// Warna: color_t + palet libs/color. Campuran lewat color_blend_alpha,
// state tombol lewat color_darken/color_lighten; coverage sudut tetap aa_cov.
// Semua integer — app dibangun -mno-sse -msoft-float.
// ------------------------------------------------------------

} // namespace ui

#endif // KWIDGET_CORE_THEME_HPP
