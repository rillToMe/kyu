// apps/taskmgr/performance_page.hpp — halaman Performance (CPU/RAM/Disk/Uptime).
//
// Label + progressbar dibuat sekali (build), nilainya diisi ulang (refresh).
// Tidak ada widget grafik kustom: libui tidak punya graph widget dan grafik
// ringan di luar pola toolkit — bar + angka sudah menjawab kebutuhan monitor.
#ifndef TASKMGR_PERFORMANCE_PAGE_HPP
#define TASKMGR_PERFORMANCE_PAGE_HPP

#include <string>

#include "platform.hpp"
#include "system_model.hpp"

namespace taskmgr {

struct PerformancePage {
    ui_widget_t* root = nullptr;
    ui_widget_t* cpu = nullptr;
    ui_widget_t* cpu_bar = nullptr;
    ui_widget_t* ram = nullptr;
    ui_widget_t* ram_bar = nullptr;
    ui_widget_t* disk = nullptr;
    ui_widget_t* disk_bar = nullptr;
    ui_widget_t* uptime = nullptr;

    void build(ui_window_t* win);
    void refresh(const SystemStats& st);
};

}  // namespace taskmgr

#endif // TASKMGR_PERFORMANCE_PAGE_HPP
