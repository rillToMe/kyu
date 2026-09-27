// apps/taskmgr/processes_page.hpp — halaman Processes (tabel + status).
//
// Page dirangkai sekali (build), diisi ulang tiap tick (refresh): widget tidak
// pernah dibuat ulang per 500 ms. Seleksi dipertahankan per-PID (bukan per
// index) supaya baris tidak "melompat" saat proses lain keluar/masuk.
//
// Kolom SENGAJA hanya PID/Name/UID/State: kernel tidak mengekspos CPU/mem
// per-proses (tidak ada di proc_info_t), jadi kolom itu tidak ditampilkan
// daripada difake.
//
// Sort (PID/Name/State) diganti lewat shortcut keyboard 'S' (libui table tidak
// punya klik-header); kunci aktif tercetak di status.
#ifndef TASKMGR_PROCESSES_PAGE_HPP
#define TASKMGR_PROCESSES_PAGE_HPP

#include <cstdint>
// <string> WAJIB sebelum header platform apa pun: deklarasi memcpy/memset/strcmp
// userlib.h (tanpa noexcept) konflik dengan string.h SDK (noexcept) bila
// userlib.h masuk duluan (urutan sebaliknya aman — pola settings::settings.hpp).
#include <string>

#include "platform.hpp"
#include "process_model.hpp"

namespace taskmgr {

struct ProcessesPage {
    ui_widget_t* root = nullptr;
    ui_widget_t* table = nullptr;
    ui_widget_t* status = nullptr;

    void build(ui_window_t* win);
    // Gambar ulang isi tabel dari model; `model` tetap milik app.
    void refresh(const ProcessModel& model);
    // PID baris terpilih, -1 bila tidak ada.
    std::int32_t selectedPid(const ProcessModel& model) const;
};

const char* sortName(SortKey key);

}  // namespace taskmgr

#endif // TASKMGR_PROCESSES_PAGE_HPP
