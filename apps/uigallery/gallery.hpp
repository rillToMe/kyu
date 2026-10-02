// apps/uigallery/gallery.hpp — aplikasi Galeri UI libui.
//
// TUJUAN (bukan sekadar demo): galeri ini adalah SATU-SATUNYA referensi visual
// yang benar untuk design system KyuzenOS. Setiap app baru bisa membuka galeri
// ini dan melihat bagaimana kontrol SEHARUSNYA tampil dan berperilaku.
//
// Prinsip yang dipegang:
//   * Memakai widget PRODUKSI dari libui (bukan komponen tiruan). Kalau sebuah
//     kontrol berubah, galeri ikut berubah — itu memang tujuannya.
//   * Setiap kontrol ditampilkan beserta STATE-nya (normal, hover, focused,
//     disabled, selected) supaya "bahasa state" bisa diperiksa mata.
//   * Navigasi memakai primitif yang sama dengan Settings (sidebar daftar),
//     jadi galeri sekaligus contoh pola aplikasi KyuzenOS.
// CATATAN BUILD: app ini dibangun dengan resep C++ toolkit (apps/app.ld,
// ENTRY(main), TANPA C++ SDK) — sama seperti gallery/imageview. Karena itu
// hanya header freestanding yang boleh dipakai: <stdint.h>, bukan <cstdint>;
// dan TIDAK BOLEH ada objek namespace-scope dengan konstruktor (ELF loader
// tidak menjalankan .init_array).
#ifndef UIGALLERY_GALLERY_HPP
#define UIGALLERY_GALLERY_HPP

#include <stdint.h>

#include "platform.hpp"

namespace gal {

// Halaman galeri (urutan = urutan sidebar).
enum class GalPage : uint8_t {
    Typography = 0,
    Colors,
    Buttons,
    Inputs,
    Selection,
    Lists,
    Navigation,
    Overlays,
    Count
};

constexpr int kPageCount = static_cast<int>(GalPage::Count);

struct NavItem {
    GalPage page;
    const char* title;
    int icon;
};

class UiGalleryApp {
public:
    UiGalleryApp();
    ~UiGalleryApp();

    int run();

    // --- Perintah publik (dipanggil thunk C / callback widget) ---
    void onNavChanged();
    void showPage(GalPage page);
    void setStatus(const char* text);
    void setMode(ui_theme_mode_t mode);
    void setAccent(ui_theme_accent_t accent);

private:
    ui_window_t* win_;
    ui_widget_t* sidebar_;
    ui_widget_t* scroll_;
    ui_widget_t* stack_;
    ui_widget_t* pages_[kPageCount];
    ui_widget_t* status_;
    ui_widget_t* mode_label_;
    ui_widget_t* accent_label_;
    GalPage current_;

    // Kontrol contoh yang statusnya dilaporkan ke status bar.
    ui_widget_t* demo_switch_;
    ui_widget_t* demo_checkbox_;
    ui_widget_t* demo_slider_;
    ui_widget_t* demo_combo_;
    ui_widget_t* demo_list_;

    ui_theme_mode_t mode_;
    ui_theme_accent_t accent_;

    void build();
    void applyTheme();
    void syncLabels();

    ui_widget_t* pageTypography(ui_window_t* w);
    ui_widget_t* pageColors(ui_window_t* w);
    ui_widget_t* pageButtons(ui_window_t* w);
    ui_widget_t* pageInputs(ui_window_t* w);
    ui_widget_t* pageSelection(ui_window_t* w);
    ui_widget_t* pageLists(ui_window_t* w);
    ui_widget_t* pageNavigation(ui_window_t* w);
    ui_widget_t* pageOverlays(ui_window_t* w);
};

}  // namespace gal

#endif // UIGALLERY_GALLERY_HPP
