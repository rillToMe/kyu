// apps/settings/appearance.cpp — implementasi halaman Appearance.
#include "appearance.hpp"

namespace settings {

// Warna ditulis per komponen (libs/gui/color) — urutan field tetap 6 warna ABI
// ui_theme_t (sama seperti versi C lama + widget_demo).
const ui_theme_t kThemeDark = {
    COLOR_RGB_INIT(0x12, 0x12, 0x12), COLOR_RGB_INIT(0xE0, 0xE0, 0xE0),
    COLOR_RGB_INIT(0xE9, 0x45, 0x60), COLOR_RGB_INIT(0x0F, 0x34, 0x60),
    COLOR_WHITE_INIT,          COLOR_RGB_INIT(0x2A, 0x4A, 0x7E),
};
const ui_theme_t kThemeLight = {
    COLOR_RGB_INIT(0xF0, 0xF0, 0xF0), COLOR_RGB_INIT(0x22, 0x22, 0x22),
    COLOR_RGB_INIT(0xD3, 0x2F, 0x2F), COLOR_RGB_INIT(0xCF, 0xD8, 0xDC),
    COLOR_RGB_INIT(0x22, 0x22, 0x22), COLOR_RGB_INIT(0x90, 0xA4, 0xAE),
};
const ui_theme_t kThemeGreen = {
    COLOR_RGB_INIT(0x0D, 0x1F, 0x14), COLOR_RGB_INIT(0xDF, 0xF2, 0xE0),
    COLOR_RGB_INIT(0x4C, 0xAF, 0x50), COLOR_RGB_INIT(0x1B, 0x4D, 0x2E),
    COLOR_RGB_INIT(0xE8, 0xF5, 0xE9), COLOR_RGB_INIT(0x2E, 0x7D, 0x46),
};

namespace {

void onTheme(void* ud) {
    AppearancePage::ThemeBinding* b =
        static_cast<AppearancePage::ThemeBinding*>(ud);
    if (b && b->page) b->page->applyTheme(b->theme, b->name);
}

}  // namespace

void AppearancePage::build(ui_window_t* w) {
    win = w;
    ui_widget_t* box = ui_vbox_create(w, 6);
    ui_layout_add(box, ui_label_create(w, "Appearance"));
    ui_layout_add(box, ui_label_create(w, "Choose how KyuzenOS looks."));

    const ui_theme_t* themes[3] = {&kThemeDark, &kThemeLight, &kThemeGreen};
    const char* names[3] = {"Dark", "Light", "Green"};
    for (int i = 0; i < 3; i++) {
        bindings[i].page = this;
        bindings[i].theme = themes[i];
        bindings[i].name = names[i];
        ui_widget_t* b = ui_button_create(w, names[i]);
        ui_button_set_click(b, onTheme, &bindings[i]);
        ui_layout_add(box, b);
    }

    status = ui_label_create(w, "Theme: (from settings.ui)");
    ui_layout_add(box, status);
    root = box;
}

void AppearancePage::applyTheme(const ui_theme_t* theme, const char* name) {
    if (!win || !theme || !name) return;
    ui_window_set_theme(win, theme);
    if (!ui_settings_save(win)) {
        ui_label_set_text(status, "Theme applied, save failed");
        return;
    }
    char msg[48];
    int k = 0;
    const char* p = "Theme: ";
    while (p[k]) { msg[k] = p[k]; k++; }
    for (int i = 0; name[i] && k < 46; i++) msg[k++] = name[i];
    msg[k] = '\0';
    ui_label_set_text(status, msg);
}

}  // namespace settings
