// apps/taskmgr/process_model.hpp — model proses (DATA + syscall, TANPA UI).
//
// Satu-satunya tempat app ini memanggil sys_proc_list/sys_kill/sys_getpid.
// Page/app hanya MEMBACA snapshot ini; tidak ada keputusan listing/kill yang
// dibuat di lapisan UI (pola fm::DirectoryModel).
//
// Batas ABI: proc_info_t (include/proc.h) dibaca di sini saja; keluar model
// hanya tipe taskmgr:: milik app. State kernel TASK_* (include/task.h, privat)
// TIDAK di-include — pemetaan angka→enum ada di .cpp dengan komentar.
#ifndef TASKMGR_PROCESS_MODEL_HPP
#define TASKMGR_PROCESS_MODEL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "platform.hpp"

namespace taskmgr {

// Cermin TASK_* kernel TANPA include header privat (angka = kontrak ABI
// sys_proc_list; lihat include/task.h: READY 0, RUNNING 1, SLEEPING 2,
// DEAD 3, BLOCKED 4, ZOMBIE 5).
enum class ProcessState : std::uint8_t {
    Ready,
    Running,
    Sleep,
    Dead,
    Block,
    Zombie,
    Unknown
};

struct Process {
    std::int32_t pid = -1;
    std::string name;
    std::uint32_t uid = 0;
    ProcessState state = ProcessState::Unknown;
    std::int32_t exit_code = 0;  // valid hanya bila state == Zombie
};

enum class SortKey : std::uint8_t { Pid, Name, State };

enum class KillResult : std::uint8_t {
    Killed,      // sys_kill == 0
    SelfDenied,  // target = task manager sendiri (ditolak di depan)
    Failed,      // sys_kill == -1 (ditolak/sudah keluar/PID tak valid —
                 // ABI tidak membedakan, jadi tidak difabrikasi)
};

class ProcessModel {
public:
    static constexpr int kMaxProcs = 16;  // = MAX_TASKS kernel

    // Snapshot ulang dari kernel. False bila sys_proc_list gagal (isi lama
    // dipertahankan agar UI tidak berkedip kosong).
    bool refresh();

    const std::vector<Process>& processes() const { return procs_; }
    int count() const { return (int)procs_.size(); }

    void sort(SortKey key);
    SortKey sortKey() const { return sort_; }

    // Index proses ber-pid `pid`, -1 bila tidak ada (untuk seleksi ulang).
    int findByPid(std::int32_t pid) const;

    KillResult kill(std::int32_t pid);

private:
    std::vector<Process> procs_;
    SortKey sort_ = SortKey::Pid;

    void applySort();
};

const char* stateName(ProcessState st);
// "ZOMBIE(125)": alasan keluar terlihat langsung (pola taskmgr.c lama).
std::string stateLabel(const Process& p);

}  // namespace taskmgr

#endif // TASKMGR_PROCESS_MODEL_HPP
