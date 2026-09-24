// apps/settings/system.hpp — halaman System (info perangkat, read-only).
//
// Data diambil dari API existing (userlib): sys_used_ram/sys_total_ram,
// get_cpu_string, sys_gpu_stats, sys_get_screen_size. Tidak ada duplikasi
// syscall logic — hanya formatting via std::string/std::to_string.
#ifndef SETTINGS_SYSTEM_HPP
#define SETTINGS_SYSTEM_HPP

#include <string>

#include "platform.hpp"

namespace settings {

// Kumpulan label halaman System. Dibangun sekali (build), diisi ulang tiap
// halaman dibuka (refresh) agar angka RAM selalu segar.
struct SystemPage {
    ui_widget_t* root = nullptr;
    ui_widget_t* ram = nullptr;
    ui_widget_t* cpu = nullptr;
    ui_widget_t* gpu = nullptr;
    ui_widget_t* screen = nullptr;

    void build(ui_window_t* win);
    void refresh();
};

std::string formatRam();
std::string formatCpu();
std::string formatGpu();
std::string formatScreen();

}  // namespace settings

#endif // SETTINGS_SYSTEM_HPP
