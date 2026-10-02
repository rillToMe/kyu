// libs/widget/include/theme/colors.hpp — token warna + kebijakan turunannya.
//
// Lapisan `theme/` adalah SATU-SATUNYA pemilik keputusan visual. Widget tidak
// boleh mengarang warna, rasio campuran, atau radius: semuanya dibaca dari
// token di sini lewat `core/theme.hpp` (palet ter-resolve).
//
// Isi file ini = KEBIJAKAN (bagaimana state diturunkan dari base), bukan palet
// jadi. Palet jadi ada di `core/theme.hpp` supaya tetap satu struct `Theme`
// yang dibaca widget (arsitektur lama dipertahankan, bukan sistem paralel).
//
// Semua integer — app dibangun `-mno-sse -msoft-float`.
#ifndef KWIDGET_THEME_COLORS_HPP
#define KWIDGET_THEME_COLORS_HPP

#include "runtime/platform.hpp"

namespace ui {

// ------------------------------------------------------------
// Shade lama (jalur legacy 6-warna). Nilai TIDAK BOLEH berubah: aplikasi
// legacy harus tampil piksel-identik (dikunci tests/host/unit/libui_theme_test).
// Contoh: darken(#1E1E1E, 42) == aa_shade(#1E1E1E, -18) == #191919.
// ------------------------------------------------------------
constexpr uint8_t SHADE_5  = 13;    // +5%  (terang)
constexpr uint8_t SHADE_10 = 25;    // ±10% (gradien tombol lama)
constexpr uint8_t SHADE_18 = 42;    // -18% (tombol ditekan)
constexpr uint8_t SHADE_55 = 139;   // -55% (item menu nonaktif)

// ------------------------------------------------------------
// Campuran opaque: t/255 bobot fg di atas bg. Tinta seleksi/subtle di atas
// permukaan (painter rect opaque — tanpa alpha blending di sini).
// ------------------------------------------------------------
static inline color_t theme_mix(color_t fg, color_t bg, uint8_t t) {
    color_t out;
    out.r = (uint8_t)(((uint32_t)fg.r * t + (uint32_t)bg.r * (255u - t)) / 255u);
    out.g = (uint8_t)(((uint32_t)fg.g * t + (uint32_t)bg.g * (255u - t)) / 255u);
    out.b = (uint8_t)(((uint32_t)fg.b * t + (uint32_t)bg.b * (255u - t)) / 255u);
    out.a = 255;
    return out;
}

// ------------------------------------------------------------
// Base aksen (0xRRGGBB). Satu warna per aksen; state (hover/pressed/subtle/
// contrast) DITURUNKAN deterministik, bukan di-hardcode per kombinasi
// mode × aksen. Biru BUKAN default — identitas KyuzenOS netral.
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

// ------------------------------------------------------------
// Rasio turunan state. Angka-angka ini adalah "aturan bahasa visual":
// hover harus terlihat tapi tidak berteriak, pressed harus jelas turun.
// Dikumpulkan di satu tempat supaya semua widget bergerak dengan takaran sama.
// ------------------------------------------------------------
namespace ratio {
// Permukaan interaktif.
constexpr uint8_t HOVER_MIX   = 22;   // surface → surface_hover (ke arah teks)
constexpr uint8_t PRESSED_MIX = 200;  // surface → surface_pressed (ke arah bg)
// surface → surface_variant (ke arah bg). Ini "well" (input, track, gutter).
//
// KENAPA KE ARAH BG, BUKAN KE ARAH TEKS: sebelumnya variant dicampur ke arah
// teks seperti hover, dan di mode gelap hasilnya 0x232323 — PERSIS sama dengan
// surface_elevated. Akibatnya input (well) dan tombol (terangkat) tampil
// dengan warna identik, dan seluruh pembeda "terbenam vs terangkat" hilang di
// mode gelap tanpa ada yang menyadarinya. Menuju bg membuat arahnya selalu
// BERLAWANAN dengan elevated di kedua mode, jadi ketiganya selalu berbeda.
// Dikunci test: variant != surface, variant != elevated (terang & gelap).
constexpr uint8_t VARIANT_MIX = 150;
// Teks.
constexpr uint8_t TERTIARY_MIX = 96;  // text → text_tertiary (placeholder/caption)
constexpr uint8_t DISABLED_MIX = 115; // text → text_disabled (jalur config)
// Seleksi.
constexpr uint8_t SELECT_DARK  = 77;
constexpr uint8_t SELECT_LIGHT = 51;
// Aksen.
constexpr uint8_t ACCENT_SUBTLE_MIX = 38;
// Aksen di atas bg untuk derivasi hover/pressed (per mode).
constexpr uint8_t ACCENT_HOVER_DARKEN_LIGHT = 22;
constexpr uint8_t ACCENT_PRESS_DARKEN_LIGHT = 42;
constexpr uint8_t ACCENT_HOVER_LIGHTEN_DARK = 32;
constexpr uint8_t ACCENT_PRESS_DARKEN_DARK  = 36;
}  // namespace ratio

// ------------------------------------------------------------
// Tone teks — "warna semantik untuk teks". Widget memilih tone, bukan warna,
// sehingga hierarki tipografi tetap konsisten di semua tema.
// ------------------------------------------------------------
enum TextTone {
    TONE_PRIMARY = 0,   // teks utama (judul, isi)
    TONE_SECONDARY,     // pendukung (deskripsi, accelerator)
    TONE_TERTIARY,      // paling redup yang masih terbaca (placeholder, hint)
    TONE_DISABLED,      // nonaktif
    TONE_ACCENT,        // teks di atas aksen (accent_contrast)
    TONE_DANGER,        // teks error
    TONE_COUNT
};

// ------------------------------------------------------------
// Mode terang/gelap → base netral. Nilai ini yang mendefinisikan "warna kertas"
// KyuzenOS: netral hangat-sedikit, tanpa cast biru.
// ------------------------------------------------------------
struct NeutralBase {
    color_t bg;
    color_t surface;
    color_t surface_elevated;
    color_t text;
    color_t text_secondary;
    color_t border;
    color_t border_subtle;
    color_t success, warning, danger, info;
};

static inline NeutralBase theme_neutral_base(bool light) {
    NeutralBase b;
    if (light) {
        b.bg              = color_hex(0xF5F5F5);
        b.surface         = color_hex(0xFFFFFF);
        b.surface_elevated = color_hex(0xEAEAEA);
        b.text            = color_hex(0x181818);
        b.text_secondary  = color_hex(0x666666);
        b.border          = color_hex(0xD6D6D6);
        b.border_subtle   = color_hex(0xE3E3E3);
        b.success         = color_hex(0x1A7F37);
        b.warning         = color_hex(0x9A6700);
        b.danger          = color_hex(0xCF222E);
        b.info            = color_hex(0x0969DA);
    } else {
        b.bg              = color_hex(0x111111);
        b.surface         = color_hex(0x181818);
        b.surface_elevated = color_hex(0x232323);
        b.text            = color_hex(0xF2F2F2);
        b.text_secondary  = color_hex(0xA8A8A8);
        b.border          = color_hex(0x303030);
        b.border_subtle   = color_hex(0x262626);
        b.success         = color_hex(0x3FB950);
        b.warning         = color_hex(0xD29922);
        b.danger          = color_hex(0xF85149);
        b.info            = color_hex(0x58A6FF);
    }
    return b;
}

// ------------------------------------------------------------
// State permukaan diturunkan dari (surface, bg, text) — TIDAK dari mode.
// Arahnya otomatis benar di kedua mode:
//   hover   = campur `text` ke `surface`  → gelap: naik, terang: turun
//   pressed = campur `bg` ke `surface`    → keduanya turun ke arah kertas
// Karena itu tidak ada tabel 2×N state di source.
// ------------------------------------------------------------
static inline color_t theme_surface_variant(color_t surface, color_t bg) {
    return theme_mix(bg, surface, ratio::VARIANT_MIX);
}
static inline color_t theme_surface_hover(color_t surface, color_t text) {
    return theme_mix(text, surface, ratio::HOVER_MIX);
}
static inline color_t theme_surface_pressed(color_t surface, color_t bg) {
    return theme_mix(bg, surface, ratio::PRESSED_MIX);
}

} // namespace ui

#endif // KWIDGET_THEME_COLORS_HPP
