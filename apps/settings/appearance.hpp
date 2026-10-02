// apps/settings/appearance.hpp — halaman Appearance (mode + aksen).
//
// MIGRASI UI: halaman ini sebelumnya memakai tiga tema 6-warna hardcoded
// (Dark/Light/Green). Sekarang memakai SISTEM TEMA yang sudah ada
// (`ui_theme_config_t`: mode × aksen) — 2 mode × 6 aksen dari satu jalur
// turunan, bukan tabel tema yang ditulis satu per satu.
//
// Tidak ada theme engine duplikat: palet tetap diturunkan libui, dan
// persistensi tetap lewat ui_settings_save/load.
#ifndef SETTINGS_APPEARANCE_HPP
#define SETTINGS_APPEARANCE_HPP

#include "platform.hpp"

namespace settings {

struct AppearancePage {
    ui_widget_t* root = nullptr;
    ui_widget_t* status = nullptr;
    ui_window_t* win = nullptr;
    ui_widget_t* mode_dark = nullptr;
    ui_widget_t* mode_light = nullptr;
    ui_radio_group_t* mode_group = nullptr;

    // Binding callback: satu per mode + satu per aksen. Member (bukan heap):
    // alamat stabil selama page hidup, tanpa operator new.
    struct ModeBinding {
        AppearancePage* page = nullptr;
        ui_theme_mode_t mode = UI_THEME_DARK;
    };
    struct AccentBinding {
        AppearancePage* page = nullptr;
        ui_theme_accent_t accent = UI_ACCENT_NEUTRAL;
        const char* name = nullptr;
    };
    ModeBinding mode_bindings[2];
    AccentBinding accent_bindings[6];

    void build(ui_window_t* w);
    // Terapkan mode + aksen aktif ke window lalu simpan.
    void apply();
    void setMode(ui_theme_mode_t mode);
    void setAccent(ui_theme_accent_t accent, const char* name);
    void syncMode();

private:
    ui_theme_mode_t mode_ = UI_THEME_DARK;
    ui_theme_accent_t accent_ = UI_ACCENT_NEUTRAL;
    const char* accent_name_ = "Neutral";
    void setStatusTheme();
};

}  // namespace settings

#endif // SETTINGS_APPEARANCE_HPP
