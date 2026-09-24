// apps/settings/appearance.hpp — halaman Appearance (preset tema).
//
// Memakai tema existing + ui_window_set_theme + ui_settings_save/load.
// Tidak ada theme engine duplikat.
#ifndef SETTINGS_APPEARANCE_HPP
#define SETTINGS_APPEARANCE_HPP

#include "platform.hpp"

namespace settings {

struct AppearancePage {
    ui_widget_t* root = nullptr;
    ui_widget_t* status = nullptr;
    ui_window_t* win = nullptr;

    // Binding callback per tombol tema. Member (bukan heap): alamat stabil
    // selama page hidup, dan tidak butuh operator new.
    struct ThemeBinding {
        AppearancePage* page = nullptr;
        const ui_theme_t* theme = nullptr;
        const char* name = nullptr;
    };
    ThemeBinding bindings[3];

    void build(ui_window_t* w);
    void applyTheme(const ui_theme_t* theme, const char* name);
};

extern const ui_theme_t kThemeDark;
extern const ui_theme_t kThemeLight;
extern const ui_theme_t kThemeGreen;

}  // namespace settings

#endif // SETTINGS_APPEARANCE_HPP
