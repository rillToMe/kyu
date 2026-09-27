// apps/taskmgr/task_manager.hpp — aplikasi Task Manager (kelas utama, pola
// settings::SettingsApp / fm::FileManagerApp / gal::Gallery: satu kelas app
// dengan perintah publik + state privat; objek dibuat DI DALAM main, bukan
// global).
//
// Pembagian lapisan:
//   platform.hpp        — header platform C (extern "C")
//   format.hpp          — helper formatting integer→std::string (header-only)
//   process_model.*     — snapshot proses + sort + kill (DATA + syscall, TANPA UI)
//   system_model.*      — snapshot metrik CPU/RAM/disk/uptime (TANPA UI)
//   processes_page.*    — tabel proses + status (build sekali, refresh per tick)
//   performance_page.*  — label + progressbar CPU/RAM/disk/uptime
//   memory_page.*       — diagnostik RAM + batas ABI yang dinyatakan
//   task_manager.*      — TaskManagerApp: window, sidebar, tick 500 ms, aksi
//   main.cpp            — entry
#ifndef TASKMGR_TASK_MANAGER_HPP
#define TASKMGR_TASK_MANAGER_HPP

#include <cstdint>
// <string> WAJIB sebelum header platform apa pun (pola settings::settings.hpp).
#include <string>

#include "memory_page.hpp"
#include "performance_page.hpp"
#include "processes_page.hpp"
#include "process_model.hpp"
#include "system_model.hpp"

namespace taskmgr {

// Kategori (urutan = urutan sidebar).
enum class TaskPage : std::uint8_t {
    Processes = 0,
    Performance,
    Memory,
    Count
};

struct NavigationItem {
    TaskPage page;
    const char* title;
};

class TaskManagerApp {
public:
    TaskManagerApp();
    ~TaskManagerApp();

    // Bangun UI + refresh awal, lalu blocking sampai window ditutup.
    // Return 0 selalu (keluar lewat sys_exit di main).
    int run();

    // --- Perintah (dipanggil sidebar/button/keyboard lewat thunk C) ---
    void showPage(TaskPage page);
    void onSidebarChanged();
    void onKey(std::uint32_t ascii);
    void onEndTask();
    void onRefresh();
    void onCycleSort();
    int onTick();

    static TaskManagerApp* self(void* ud) {
        return static_cast<TaskManagerApp*>(ud);
    }

private:
    ui_window_t* win_;
    ui_widget_t* sidebar_;
    TaskPage current_;
    std::uint64_t last_tick_ms_;

    // Model + page dirangkai by value (bukan new): hidup mengikuti app, alamat
    // stabil untuk userdata callback widget.
    ProcessModel procs_;
    SystemModel sys_;
    ProcessesPage processes_;
    PerformancePage performance_;
    MemoryPage memory_;

    void buildUi();
    void refreshAll();
    void setStatus(const char* text);
    void setStatusStr(const std::string& text);
};

}  // namespace taskmgr

#endif // TASKMGR_TASK_MANAGER_HPP
