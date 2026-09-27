// apps/taskmgr/process_model.cpp — implementasi model proses.
#include "process_model.hpp"

#include <algorithm>

#include "format.hpp"

namespace taskmgr {

namespace {

// TASK_* kernel (include/task.h, privat — tidak di-include; angka disalin
// sebagai kontrak ABI sys_proc_list).
constexpr std::uint8_t kReady = 0;
constexpr std::uint8_t kRunning = 1;
constexpr std::uint8_t kSleeping = 2;
constexpr std::uint8_t kDead = 3;
constexpr std::uint8_t kBlocked = 4;
constexpr std::uint8_t kZombie = 5;

ProcessState fromRaw(std::uint8_t st) {
    switch (st) {
        case kReady: return ProcessState::Ready;
        case kRunning: return ProcessState::Running;
        case kSleeping: return ProcessState::Sleep;
        case kDead: return ProcessState::Dead;
        case kBlocked: return ProcessState::Block;
        case kZombie: return ProcessState::Zombie;
        default: return ProcessState::Unknown;
    }
}

}  // namespace

const char* stateName(ProcessState st) {
    switch (st) {
        case ProcessState::Ready: return "READY";
        case ProcessState::Running: return "RUNNING";
        case ProcessState::Sleep: return "SLEEP";
        case ProcessState::Dead: return "DEAD";
        case ProcessState::Block: return "BLOCK";
        case ProcessState::Zombie: return "ZOMBIE";
        default: return "?";
    }
}

std::string stateLabel(const Process& p) {
    std::string s(stateName(p.state));
    if (p.state == ProcessState::Zombie)
        s = s + "(" + toStr32(p.exit_code) + ")";
    return s;
}

bool ProcessModel::refresh() {
    proc_info_t list[kMaxProcs];
    const int n = sys_proc_list(list, kMaxProcs);
    if (n < 0) return false;
    std::vector<Process> next;
    for (int i = 0; i < n && i < kMaxProcs; i++) {
        Process p;
        p.pid = list[i].pid;
        p.uid = list[i].uid;
        p.state = fromRaw(list[i].state);
        p.exit_code = list[i].exit_code;
        // name[16] kernel belum tentu NUL-terminated bila penuh: batasi.
        char name[17];
        for (int k = 0; k < 16; k++) name[k] = list[i].name[k];
        name[16] = '\0';
        p.name = name;
        next.push_back(p);
    }
    procs_ = next;
    applySort();
    return true;
}

void ProcessModel::sort(SortKey key) {
    sort_ = key;
    applySort();
}

void ProcessModel::applySort() {
    // std::sort subset Kyuzen: komparator integer/char saja (tanpa sort FP).
    // Pembanding nama memakai operator< std::string (berbasis char) — aman.
    switch (sort_) {
        case SortKey::Name:
            std::sort(procs_.begin(), procs_.end(),
                      [](const Process& a, const Process& b) {
                          if (a.name != b.name) return a.name < b.name;
                          return a.pid < b.pid;
                      });
            break;
        case SortKey::State:
            std::sort(procs_.begin(), procs_.end(),
                      [](const Process& a, const Process& b) {
                          const int sa = (int)a.state, sb = (int)b.state;
                          if (sa != sb) return sa < sb;
                          return a.pid < b.pid;
                      });
            break;
        case SortKey::Pid:
        default:
            std::sort(procs_.begin(), procs_.end(),
                      [](const Process& a, const Process& b) {
                          return a.pid < b.pid;
                      });
            break;
    }
}

int ProcessModel::findByPid(std::int32_t pid) const {
    for (int i = 0; i < (int)procs_.size(); i++)
        if (procs_[i].pid == pid) return i;
    return -1;
}

KillResult ProcessModel::kill(std::int32_t pid) {
    // Bunuh diri dilarang di depan (UX jelas; kernel pun menolak via
    // proc_can_kill? tidak — suicide justru diizinkan kernel, jadi guard ini
    // milik app dan memang disengaja).
    if (pid == sys_getpid()) return KillResult::SelfDenied;
    return sys_kill(pid) == 0 ? KillResult::Killed : KillResult::Failed;
}

}  // namespace taskmgr
