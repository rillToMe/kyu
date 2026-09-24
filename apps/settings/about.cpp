// apps/settings/about.cpp — implementasi halaman About.
#include "about.hpp"

#include "kyuzen_version.h"  // versi kanonis (single source of truth)

namespace settings {

void AboutPage::build(ui_window_t* win) {
    ui_widget_t* box = ui_vbox_create(win, 6);
    ui_layout_add(box, ui_label_create(win, "About"));
    ui_layout_add(box, ui_label_create(win, "KyuzenOS"));
    ui_layout_add(box, ui_label_create(win, "Version"));
    ui_layout_add(box, ui_label_create(win, KYUZEN_VERSION));
    ui_layout_add(box, ui_label_create(win, "Architecture: x86_64"));
    ui_layout_add(box, ui_label_create(
                           win, "Kernel: 64-bit higher-half monolithic"));
    ui_layout_add(box, ui_label_create(win, "Boot: Limine"));
    gpu_stats_t st;
    if (sys_gpu_stats(&st) == 0)
        ui_layout_add(box, ui_label_create(win, "Graphics: accelerated"));
    else
        ui_layout_add(box, ui_label_create(win, "Graphics: software"));
    ui_layout_add(box, ui_label_create(
                           win, "Made for experimentation and learning."));
    root = box;
}

}  // namespace settings
