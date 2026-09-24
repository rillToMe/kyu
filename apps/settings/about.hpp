// apps/settings/about.hpp — halaman About.
//
// Hanya info yang memang tersedia (tanpa hardcode yang seharusnya dari
// system): arsitektur x86_64, kernel monolitik 64-bit higher-half + Limine,
// backend grafis dari sys_gpu_stats. TIDAK ada version API di repo ini, jadi
// versi tidak ditampilkan (dilaporkan, bukan difake).
#ifndef SETTINGS_ABOUT_HPP
#define SETTINGS_ABOUT_HPP

#include "platform.hpp"

namespace settings {

struct AboutPage {
    ui_widget_t* root = nullptr;

    void build(ui_window_t* win);
};

}  // namespace settings

#endif // SETTINGS_ABOUT_HPP
