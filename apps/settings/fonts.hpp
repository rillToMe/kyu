// apps/settings/fonts.hpp — halaman Fonts.
//
// FontSettings: satu-satunya pemilik font preview (buffer + face). Semua
// mutasi lewat method-nya; gagal muat -> fallback status (bukan crash).
// Persistence = font.ui via kzfontcfg.h (format existing, bukan format baru).
#ifndef SETTINGS_FONTS_HPP
#define SETTINGS_FONTS_HPP

#include "platform.hpp"

namespace settings {

class FontSettings {
public:
    FontSettings();
    ~FontSettings();

    bool load(int id);       // muat font id dari FS root (sanitize otomatis)
    bool save();             // persist id saat ini ke font.ui
    bool loadSaved();        // baca font.ui -> load (absen/rusak = default)
    const char* currentName() const;
    int currentId() const { return index_; }
    kz_font_t* font() { return font_; }

private:
    void freeFont();

    kz_font_t* font_;
    char* data_;
    int index_;
};

struct FontsPage {
    ui_widget_t* root = nullptr;
    ui_widget_t* preview = nullptr;  // FtText (toolkit; draw via callback)
    ui_widget_t* status = nullptr;
    ui_window_t* win = nullptr;
    FontSettings fonts;

    // Binding callback per tombol font. Member (bukan heap): alamat stabil.
    struct FontBinding {
        FontsPage* page = nullptr;
        int id = -1;
    };
    FontBinding bindings[KZ_FONT_COUNT];

    void build(ui_window_t* w);
    void select(int id);
    void save();
    void load();
};

}  // namespace settings

#endif // SETTINGS_FONTS_HPP
