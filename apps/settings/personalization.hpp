// apps/settings/personalization.hpp — halaman Personalization (wallpaper).
//
// Pilihan disimpan ke "/wallpaper.ui" (bukan /apps/desktop.app — berkas itu
// ditulis ulang kernel dari modul ISO setiap boot). Desktop membaca berkas
// ini saat start maupun saat event HOT_RELOAD(WALLPAPER) via sys_hot_reload
// generik (syscall 85, target di kwm_abi.h).
// Alur: pilih -> preview (ui_image_*) -> terapkan (persist + hot reload).
//
// Daftar wallpaper = WALL_BUILTINS milik desktop (system/desktop/theme.hpp),
// diduplikasi di sini sebagai daftar nama saja (bukan logika decode).
#ifndef SETTINGS_PERSONALIZATION_HPP
#define SETTINGS_PERSONALIZATION_HPP

#include "platform.hpp"

namespace settings {

struct PersonalizationPage {
    static constexpr int kWallpaperCount = 6;

    ui_widget_t* root = nullptr;
    ui_widget_t* preview = nullptr;  // ui::Image (toolkit pemilik buffer)
    ui_widget_t* status = nullptr;
    ui_window_t* win = nullptr;

    // Binding callback per wallpaper. Member (bukan heap): alamat stabil.
    struct WallpaperBinding {
        PersonalizationPage* page = nullptr;
        int index = -1;
    };
    WallpaperBinding bindings[kWallpaperCount];

    const char* selected = nullptr;  // nama wallpaper terpilih (preview)

    void build(ui_window_t* w);
    void select(int index);
    void cycleNext();  // pratinjau wallpaper berikutnya (shortcut headless)
    void apply();

    // Manifest helpers (murni logika + syscall FS, tanpa widget).
    static const char* builtinName(int index);
    static int builtinIndex(const char* name);
    static void readSaved(char* out, int cap);
    static bool writeSaved(const char* name);
};

}  // namespace settings

#endif // SETTINGS_PERSONALIZATION_HPP
