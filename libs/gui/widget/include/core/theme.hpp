// libs/widget/include/core/theme.hpp — palet tema aktif (mode × aksen).
//
// ARSITEKTUR (dipertahankan, bukan diganti):
//   ThemeConfig (ui_theme_config_t: mode × aksen) → palet semantik → widget.
//   Jalur legacy (ui_theme_t 6 warna) tetap didukung: 6 field lama diisi persis
//   seperti dulu + role semantik dipetakan 1:1 dari nilainya, sehingga aplikasi
//   legacy tampil identik piksel-per-piksel.
//
// YANG BARU (design system KyuzenOS):
//   * `struct Theme` kini juga membawa `Metrics` dan `Typography` — jadi satu
//     objek tema memuat SELURUH bahasa visual (warna + jarak + tipografi).
//     Widget membacanya lewat `p.theme`, tidak ada lagi angka ajaib di widget.
//   * Role permukaan diperluas: surface_variant / surface_hover /
//     surface_pressed / surface_selected. Widget memilih STATE, bukan warna.
//   * `tone()` menerjemahkan peran tipografi → warna teks, sehingga hirarki
//     teks konsisten di semua widget dan semua mode.
//
// Semua color_t (libs/color), integer-only (app dibangun -mno-sse -msoft-float).
#ifndef KWIDGET_CORE_THEME_HPP
#define KWIDGET_CORE_THEME_HPP

#include "runtime/platform.hpp"
#include "theme/theme.hpp"

namespace ui {

// ------------------------------------------------------------
// Theme — field legacy (6 + 8, NAMA DAN MAKNA TAK BERUBAH) + peran semantik
// + token design system (Metrics/Typography).
//
// Peran semantik dan pemiliknya:
//   surface / surface_variant / surface_elevated — permukaan diam
//   surface_hover / surface_pressed              — state interaktif
//   surface_selected                             — baris terpilih
//   text / text_secondary / text_tertiary / text_disabled — teks
//   border / border_subtle                       — garis tepi / pemisah
//   accent* — aksen + turunannya (aksi primer, slider, fokus)
//   success / warning / danger / info            — diagnostik
// ------------------------------------------------------------
struct Theme {
    // Legacy: 6 warna dasar ui_theme_t + 8 lapisan turunan.
    color_t bg, fg, accent, button_bg, button_fg, button_hover;
    color_t editor, chrome, panel, btnfill, divider, mborder, acc_text, caret;
    // Semantik — permukaan.
    //   surface         permukaan diam (tombol, input, panel)
    //   surface_variant permukaan TERBENAM (well/track: slider, progress,
    //                   gutter, area kosong) — kebalikan arah dari elevated
    //   surface_elevated permukaan TERANGKAT (hover, header, tab aktif)
    //   surface_hover / surface_pressed — state interaktif, diturunkan
    //   selection       latar baris terpilih (nama lama dipertahankan;
    //                   inilah token "surface_selected")
    color_t surface, surface_elevated, surface_variant;
    color_t surface_hover, surface_pressed;
    // Semantik — teks.
    color_t text, text_secondary, text_tertiary, text_disabled;
    // Semantik — garis.
    color_t border, border_subtle;
    // Semantik — aksen.
    color_t accent_hover, accent_pressed, accent_subtle, accent_contrast,
            accent_text;
    color_t selection, focus;
    // Semantik — status.
    color_t success, warning, danger, info;

    // Design system: jarak/radius/ukuran + peran tipografi.
    Metrics metrics;
    Typography type;

    // Default = Dark + Neutral. TIDAK ada ketergantungan biru di tema bawaan.
    Theme() {
        static const ui_theme_config_t def = { UI_THEME_DARK, UI_ACCENT_NEUTRAL,
                                               { 0, 0, 0, 255 } };
        apply_config(&def);
    }

    // --------------------------------------------------------
    // Jalur legacy: 6 warna eksplisit aplikasi. Field legacy diisi persis
    // seperti dulu (derive_legacy); peran semantik dipetakan 1:1 dari nilai
    // yang sama sehingga widget tampil identik.
    // --------------------------------------------------------
    void set(const ui_theme_t* t) {
        // ABI app = color_t; alpha dipaksa opaque. Wajib: warna tema dipakai
        // sebagai dst/latar pencampuran, dan color_blend_alpha membaca
        // dst.a == 0 sebagai "kanvas kosong".
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
        text_tertiary = theme_mix(fg, bg, ratio::TERTIARY_MIX);
        // Sama persis dengan rumus menu-disabled lama (piksel-identik).
        text_disabled = color_darken(fg, SHADE_55);
        border = mborder;
        border_subtle = divider;
        derive_accent_family(accent, false);
        // Status: tetap seperti semula (tidak dipakai widget legacy).
        success = color_hex(0x3FB950);
        warning = color_hex(0xD29922);
        danger = color_hex(0xF85149);
        info = color_hex(0x58A6FF);
        derive_states(false);
    }

    // --------------------------------------------------------
    // Jalur config: mode × aksen → palet semantik penuh; field legacy
    // diisi DARI peran semantik (kompatibel dibaca/ditulis format lama).
    // --------------------------------------------------------
    void apply_config(const ui_theme_config_t* c) {
        bool light = c && c->mode == UI_THEME_LIGHT;
        ui_theme_accent_t a = c ? c->accent : UI_ACCENT_NEUTRAL;
        color_t custom = c ? c->custom : color_hex(0x000000);
        color_t abase = theme_accent_base(a, custom);

        NeutralBase nb = theme_neutral_base(light);
        bg = nb.bg;
        surface = nb.surface;
        surface_elevated = nb.surface_elevated;
        text = nb.text;
        text_secondary = nb.text_secondary;
        text_tertiary = theme_mix(text, bg, ratio::TERTIARY_MIX);
        text_disabled = theme_mix(text, bg, ratio::DISABLED_MIX);
        border = nb.border;
        border_subtle = nb.border_subtle;
        success = nb.success;
        warning = nb.warning;
        danger = nb.danger;
        info = nb.info;

        accent = abase;
        derive_accent_family(abase, light);
        focus = accent;
        // Tinta seleksi: aksen di atas background (terbaca di kedua mode,
        // berbeda dari permukaan tombol).
        selection = theme_mix(accent, bg,
                              light ? ratio::SELECT_LIGHT : ratio::SELECT_DARK);
        derive_states(light);

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

    // --------------------------------------------------------
    // State permukaan diturunkan dari (surface, bg, text) — bukan dari mode.
    // Dipakai KEDUA jalur (config & legacy) supaya widget tidak pernah
    // bergantung pada jalur mana yang aktif.
    // --------------------------------------------------------
    void derive_states(bool light) {
        (void)light;
        surface_variant = theme_surface_variant(surface, bg);
        surface_hover   = theme_surface_hover(surface, text);
        surface_pressed = theme_surface_pressed(surface, bg);
    }

    // Keluarga aksen dari satu base + mode.
    void derive_accent_family(color_t abase, bool light) {
        if (light) {
            accent_hover = color_darken(abase, ratio::ACCENT_HOVER_DARKEN_LIGHT);
            accent_pressed = color_darken(abase, ratio::ACCENT_PRESS_DARKEN_LIGHT);
        } else {
            accent_hover = color_lighten(abase, ratio::ACCENT_HOVER_LIGHTEN_DARK);
            accent_pressed = color_darken(abase, ratio::ACCENT_PRESS_DARKEN_DARK);
        }
        accent_subtle = theme_mix(abase, bg, ratio::ACCENT_SUBTLE_MIX);
        accent_contrast = color_get_contrast_text(abase);
        // Teks beraksen di atas permukaan (bukan di atas aksen) — mis. label
        // "aktif" atau nilai yang ditonjolkan. Dipilih agar tetap terbaca
        // tanpa perlu tahu mode.
        accent_text = light ? color_darken(abase, 30) : color_lighten(abase, 30);
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
    // set() sebagai shim kompatibilitas — aplikasi legacy tampil identik.
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

    // --------------------------------------------------------
    // Design system — pembacaan token. Widget memanggil ini, bukan memilih
    // warna sendiri.
    // --------------------------------------------------------

    // Permukaan berdasarkan level kedalaman (theme/elevation.hpp).
    color_t surface_for(int level) const {
        switch (level) {
        case ELEV_BASE:    return bg;
        case ELEV_POPUP:   return panel;
        case ELEV_DIALOG:  return panel;
        case ELEV_RAISED:  return surface_elevated;
        case ELEV_SURFACE:
        default:           return surface;
        }
    }

    // Warna teks untuk sebuah peran tipografi (TypeRole::tone).
    color_t tone(int t) const {
        switch (t) {
        case TONE_SECONDARY: return text_secondary;
        case TONE_TERTIARY:  return text_tertiary;
        case TONE_DISABLED:  return text_disabled;
        case TONE_ACCENT:    return accent_contrast;
        case TONE_DANGER:    return danger;
        case TONE_PRIMARY:
        default:             return text;
        }
    }

    // Teks untuk peran + state disabled (satu jalur untuk semua widget:
    // disabled SELALU menang atas tone peran).
    color_t tone_for(const TypeRole& r, bool enabled) const {
        return enabled ? tone(r.tone) : text_disabled;
    }
};

} // namespace ui

#endif // KWIDGET_CORE_THEME_HPP
