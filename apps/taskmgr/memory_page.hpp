// apps/taskmgr/memory_page.hpp — halaman Memory (diagnostik RAM).
//
// Menampilkan yang memang tersedia: used/total/free + persen (dari
// sys_total_ram/sys_used_ram). Rincian kategori (kernel/proses/page table/
// cache) TIDAK diekspos kernel — halaman menyatakan itu apa adanya (label
// statis), bukan angka fake.
#ifndef TASKMGR_MEMORY_PAGE_HPP
#define TASKMGR_MEMORY_PAGE_HPP

#include <string>

#include "platform.hpp"
#include "system_model.hpp"

namespace taskmgr {

struct MemoryPage {
    ui_widget_t* root = nullptr;
    ui_widget_t* summary = nullptr;
    ui_widget_t* bar = nullptr;
    ui_widget_t* used = nullptr;
    ui_widget_t* free = nullptr;
    ui_widget_t* total = nullptr;

    void build(ui_window_t* win);
    void refresh(const SystemStats& st);
};

}  // namespace taskmgr

#endif // TASKMGR_MEMORY_PAGE_HPP
