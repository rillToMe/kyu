// apps/settings/about.cpp — implementasi halaman About.
#include "about.hpp"

#include "kyuzen_version.h"  // versi kanonis (single source of truth)

namespace settings {

void AboutPage::build(ui_window_t* win) {
    ui_widget_t* box = ui_vbox_create(win, UI_SPACE_LG);
    ui_layout_add(box, ui_label_create(win, "About"));
    ui_layout_add(box, ui_label_create(win, "KyuzenOS"));

    ui_widget_t* sec = ui_section_create(win, "Version", UI_SPACE_SM);
    ui_layout_add(sec, ui_label_create(win, KYUZEN_VERSION));
    ui_layout_add(box, sec);

    ui_widget_t* sec2 = ui_section_create(win, "Platform", UI_SPACE_SM);
    ui_layout_add(sec2, ui_label_create(win, "Architecture: x86_64"));
    ui_layout_add(sec2,
                  ui_label_create(win, "Kernel: 64-bit higher-half monolithic"));
    ui_layout_add(sec2, ui_label_create(win, "Boot: Limine"));
    gpu_stats_t st;
    if (sys_gpu_stats(&st) == 0)
        ui_layout_add(sec2, ui_label_create(win, "Graphics: accelerated"));
    else
        ui_layout_add(sec2, ui_label_create(win, "Graphics: software"));
    ui_layout_add(box, sec2);

    ui_widget_t* sec3 = ui_section_create(win, "Project", UI_SPACE_SM);
    ui_layout_add(sec3, ui_label_create(
                            win, "Made for experimentation and learning."));
    ui_layout_add(box, sec3);
    root = box;
}

}  // namespace settings
