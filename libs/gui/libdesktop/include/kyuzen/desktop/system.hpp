// libdesktop — layanan sistem untuk aplikasi desktop (header-only API,
// implementasi di backend; nomor syscall/register TIDAK terekspos).
//
// Aturan: implementasi desktop boleh memakai wrapper C SDK (sys_*) untuk
// kebutuhan di luar framework (mis. scan FS launcher), tetapi TIDAK boleh
// menyentuh ABI mentah. Yang umum (waktu, spawn, keluar, notifikasi crash)
// disediakan di sini agar seragam.
#ifndef KYUZEN_DESKTOP_SYSTEM_HPP
#define KYUZEN_DESKTOP_SYSTEM_HPP

#include <stdint.h>

namespace kyuzen {
namespace desktop {

// Ringkasan crash milik framework (salinan field yang dipakai desktop;
// layout struct C kernel tetap di backend, tidak bocor ke sini).
struct CrashReport {
    bool pending;
    uint32_t crash_count;
    uint64_t uptime_ms;
    uint64_t vector;  // 0xFFFF = kernel_panic() manual
    uint32_t task_id;
    char task_name[16];
    char path[32];
};

class System {
public:
    // Milidetik sejak boot. Untuk timeout/polling.
    static uint64_t uptime_ms();
    // Menyerahkan CPU. Dipanggil framework tiap iterasi; aplikasi biasa
    // tidak perlu memanggilnya sendiri.
    static void yield();
    // Jalankan ELF sebagai task baru. path = "/apps/nama.elf".
    // Mengembalikan task id (>= 0) atau -1 bila gagal.
    static int spawn(const char* path);
    // Keluar dengan kode. TIDAK PERNAH kembali.
    static void exit(int code);
    // Isi `out` dari laporan crash kernel. true = boot ini menerbitkan
    // laporan crash BARU (tampilkan notifikasi sekali); false = boot normal.
    static bool poll_crash(CrashReport& out);
};

}  // namespace desktop
}  // namespace kyuzen

#endif
