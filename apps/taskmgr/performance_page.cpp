// apps/taskmgr/performance_page.cpp — implementasi halaman Performance.
#include "performance_page.hpp"

#include "format.hpp"

namespace taskmgr {

namespace {

// "CPU: 12 / 100 %"
std::string cpuLabel(const CpuStats& cpu) {
    return std::string("CPU: ") + toStr((unsigned long)cpu.usage) + " / 100 %";
}

// "RAM: 145 MB / 28766 MB"
std::string ratioMbLabel(const char* pre, std::uint64_t used,
                         std::uint64_t total) {
    return std::string(pre) + mbStr(used) + " / " + mbStr(total);
}

}  // namespace

void PerformancePage::build(ui_window_t* win) {
    ui_widget_t* box = ui_vbox_create(win, 6);
    ui_layout_add(box, ui_label_create(win, "Performance"));
    ui_layout_add(box, ui_label_create(win, "CPU"));
    cpu = ui_label_create(win, "-");
    ui_layout_add(box, cpu);
    cpu_bar = ui_progressbar_create(win, 500);
    ui_layout_add(box, cpu_bar);
    ui_layout_add(box, ui_label_create(win, "Memory"));
    ram = ui_label_create(win, "-");
    ui_layout_add(box, ram);
    ram_bar = ui_progressbar_create(win, 500);
    ui_layout_add(box, ram_bar);
    ui_layout_add(box, ui_label_create(win, "Disk"));
    disk = ui_label_create(win, "-");
    ui_layout_add(box, disk);
    disk_bar = ui_progressbar_create(win, 500);
    ui_layout_add(box, disk_bar);
    ui_layout_add(box, ui_label_create(win, "Uptime"));
    uptime = ui_label_create(win, "-");
    ui_layout_add(box, uptime);
    root = box;
}

void PerformancePage::refresh(const SystemStats& st) {
    if (!root) return;
    const std::string c = cpuLabel(st.cpu);
    ui_label_set_text(cpu, c.c_str());
    ui_progressbar_set_value(cpu_bar, (int)st.cpu.usage);

    const std::string r = ratioMbLabel("RAM: ", st.memory.used, st.memory.total);
    ui_label_set_text(ram, r.c_str());
    ui_progressbar_set_value(ram_bar, st.memory.percent());

    const std::string d = ratioMbLabel("Disk: ", st.disk.used, st.disk.total);
    ui_label_set_text(disk, d.c_str());
    ui_progressbar_set_value(disk_bar, st.disk.percent());

    const std::string u =
        std::string("Uptime: ") + toStr((unsigned long)(st.uptime_ms / 1000)) +
        " s";
    ui_label_set_text(uptime, u.c_str());
}

}  // namespace taskmgr
