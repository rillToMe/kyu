// apps/taskmgr/system_model.cpp — implementasi model metrik sistem.
#include "system_model.hpp"

namespace taskmgr {

void SystemModel::refresh() {
    stats_.memory.total = sys_total_ram();
    stats_.memory.used = sys_used_ram();
    stats_.memory.free = stats_.memory.total > stats_.memory.used
                             ? stats_.memory.total - stats_.memory.used
                             : 0;
    stats_.cpu.usage = sys_get_cpu_usage();
    stats_.disk.total = sys_get_total_disk();
    stats_.disk.used = sys_get_used_disk();
    stats_.uptime_ms = sys_uptime();
}

}  // namespace taskmgr
