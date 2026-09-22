// libdesktop backend — layanan System di atas wrapper C SDK.
#include <kyuzen/desktop/system.hpp>

extern "C" {
#include "userlib.h"
}

namespace kyuzen {
namespace desktop {

uint64_t System::uptime_ms() {
    return sys_uptime();
}

void System::yield() {
    sys_yield();
}

int System::spawn(const char* path) {
    // sys_spawn tidak menulis ke path (kernel menyalin nama); cast aman.
    return sys_spawn(const_cast<char*>(path));
}

void System::exit(int code) {
    sys_exit_code(code);
    while (true) {
    }
}

bool System::poll_crash(CrashReport& out) {
    crash_notice_t raw;
    for (unsigned i = 0; i < sizeof(raw); i++)
        reinterpret_cast<char*>(&raw)[i] = 0;
    if (sys_crash_notice(&raw) != 1 || !raw.pending) return false;
    out.pending = true;
    out.crash_count = raw.crash_count;
    out.uptime_ms = raw.uptime_ms;
    out.vector = raw.vector;
    out.task_id = raw.task_id;
    for (int i = 0; i < 15; i++) {
        out.task_name[i] = raw.task_name[i];
        if (!raw.task_name[i]) break;
    }
    out.task_name[15] = '\0';
    for (int i = 0; i < 31; i++) {
        out.path[i] = raw.path[i];
        if (!raw.path[i]) break;
    }
    out.path[31] = '\0';
    return true;
}

}  // namespace desktop
}  // namespace kyuzen
