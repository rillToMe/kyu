// apps/taskmgr/system_model.hpp — model metrik sistem (DATA + syscall, TANPA UI).
//
// Satu-satunya tempat app ini memanggil sys_total_ram/sys_used_ram/
// sys_get_cpu_usage/sys_get_total_disk/sys_get_used_disk/sys_uptime.
// Page hanya MEMBACA snapshot ini.
//
// SENGAJA tidak ada rincian kategori memori (kernel/proses/page table/cache):
// kernel hanya mengekspos total+terpakai (PMM counter dalam, tanpa ABI).
// Menampilkan kategori tanpa data = fake telemetry — dilarang.
#ifndef TASKMGR_SYSTEM_MODEL_HPP
#define TASKMGR_SYSTEM_MODEL_HPP

#include <cstdint>

#include "platform.hpp"

namespace taskmgr {

struct MemoryStats {
    std::uint64_t total = 0;
    std::uint64_t used = 0;
    std::uint64_t free = 0;
    // Persen integer (tanpa FP): 0..100.
    int percent() const {
        if (!total) return 0;
        return (int)(used * 100 / total);
    }
};

struct CpuStats {
    std::uint32_t usage = 0;  // 0..100
};

struct DiskStats {
    std::uint64_t total = 0;
    std::uint64_t used = 0;
    int percent() const {
        if (!total) return 0;
        return (int)(used * 100 / total);
    }
};

struct SystemStats {
    MemoryStats memory;
    CpuStats cpu;
    DiskStats disk;
    std::uint64_t uptime_ms = 0;
};

class SystemModel {
public:
    // Baca semua metrik sekaligus (satu refresh = satu pasang syscall).
    void refresh();
    const SystemStats& stats() const { return stats_; }

private:
    SystemStats stats_;
};

}  // namespace taskmgr

#endif // TASKMGR_SYSTEM_MODEL_HPP
