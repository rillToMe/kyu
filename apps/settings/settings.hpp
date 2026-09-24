// apps/settings/settings.hpp — aplikasi Settings (kelas utama, pola
// fm::FileManagerApp / gal::Gallery: satu kelas app dengan perintah publik +
// state privat; objek dibuat DI DALAM main, bukan global).
//
// Pembagian lapisan:
//   platform.hpp        — header platform C (extern "C")
//   system.*            — halaman System (info perangkat, read-only)
//   appearance.*        — halaman Appearance (preset tema)
//   personalization.*   — halaman Personalization (wallpaper + preview)
//   fonts.*             — halaman Fonts (FontSettings + preview live)
//   about.*             — halaman About (info statis yang memang tersedia)
//   settings.*          — SettingsApp: window, sidebar navigasi, page switching
//   main.cpp            — entry
#ifndef SETTINGS_SETTINGS_HPP
#define SETTINGS_SETTINGS_HPP

#include <cstdint>
// <string> WAJIB sebelum header platform apa pun: deklarasi memcpy/memset/strcmp
// userlib.h (tanpa noexcept) konflik dengan string.h SDK (noexcept) bila
// userlib.h masuk duluan (urutan sebaliknya aman — pola fm::fs.hpp).
#include <string>

#include "about.hpp"
#include "appearance.hpp"
#include "fonts.hpp"
#include "personalization.hpp"
#include "platform.hpp"
#include "system.hpp"

namespace settings {

// Kategori settings (urutan = urutan sidebar).
enum class SettingsPage : std::uint8_t {
    System = 0,
    Personalization,
    Appearance,
    Fonts,
    About,
    Count
};

struct NavigationItem {
    SettingsPage page;
    const char* title;
};

class SettingsApp {
public:
    SettingsApp();
    ~SettingsApp();

    // Bangun UI + muat state tersimpan, lalu blocking sampai window ditutup.
    // Return 0 selalu (keluar lewat sys_exit di main).
    int run();

    // --- Perintah (dipanggil sidebar/shortcut lewat thunk C) ---
    void showPage(SettingsPage page);
    void onSidebarChanged();
    void onKey(std::uint32_t ascii);

    static SettingsApp* self(void* ud) {
        return static_cast<SettingsApp*>(ud);
    }

private:
    ui_window_t* win_;
    ui_widget_t* sidebar_;  // ListView navigasi kategori
    SettingsPage current_;

    // Page dirangkai by value (bukan new): hidup mengikuti app, alamat stabil
    // untuk userdata callback widget.
    SystemPage system_;
    PersonalizationPage personalization_;
    AppearancePage appearance_;
    FontsPage fonts_;
    AboutPage about_;

    void buildUi();
    void trace(const char* tag) const;
};

}  // namespace settings

#endif // SETTINGS_SETTINGS_HPP
