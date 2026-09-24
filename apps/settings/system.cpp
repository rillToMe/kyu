// apps/settings/system.cpp — implementasi halaman System.
#include "system.hpp"

namespace settings {

namespace {

// std::to_string TIDAK tersedia di subset libc++ Kyuzen (libcxxrt.a hanya
// menginstansiasi basic_string<char>; stof-family butuh FP/SSE yang dimatikan
// — lihat libs/c/libc-port/src/kyuzen_libcxx_string_inst.cpp). Helper lokal
// ini + std::string operator+ adalah satu-satunya formatting yang dipakai.
void appendUlong(std::string& s, unsigned long v) {
    char t[24];
    int i = 0;
    if (v == 0) {
        s += '0';
        return;
    }
    while (v > 0 && i < 23) {
        t[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) s += t[--i];
}

std::string ulongStr(unsigned long v) {
    std::string s;
    appendUlong(s, v);
    return s;
}

}  // namespace

std::string formatRam() {
    const unsigned long used = (unsigned long)(sys_used_ram() / 1024 / 1024);
    const unsigned long tot = (unsigned long)(sys_total_ram() / 1024 / 1024);
    return ulongStr(used) + " MB used / " + ulongStr(tot) + " MB";
}

std::string formatCpu() {
    char cpu[49];
    for (int i = 0; i < 49; i++) cpu[i] = '\0';
    get_cpu_string(cpu);
    cpu[48] = '\0';
    if (!cpu[0]) return "(unknown)";
    return std::string(cpu);
}

std::string formatGpu() {
    gpu_stats_t st;
    for (unsigned i = 0; i < sizeof(st) / sizeof(unsigned char); i++)
        ((unsigned char*)&st)[i] = 0;
    if (sys_gpu_stats(&st) != 0) return "-";
    return ulongStr((unsigned long)st.present_count) + " present / " +
           ulongStr((unsigned long)st.cmd_count) + " cmd / " +
           ulongStr((unsigned long)st.notify_count) + " notify";
}

std::string formatScreen() {
    std::uint32_t w = 0, h = 0;
    if (sys_get_screen_size(&w, &h) != 0) return "-";
    return ulongStr(w) + " x " + ulongStr(h);
}

void SystemPage::build(ui_window_t* win) {
    ui_widget_t* box = ui_vbox_create(win, 6);
    ui_layout_add(box, ui_label_create(win, "System"));
    ui_layout_add(box, ui_label_create(win, "Device information"));
    ui_layout_add(box, ui_label_create(win, "RAM"));
    ram = ui_label_create(win, "-");
    ui_layout_add(box, ram);
    ui_layout_add(box, ui_label_create(win, "Processor"));
    cpu = ui_label_create(win, "-");
    ui_layout_add(box, cpu);
    ui_layout_add(box, ui_label_create(win, "Graphics"));
    gpu = ui_label_create(win, "-");
    ui_layout_add(box, gpu);
    ui_layout_add(box, ui_label_create(win, "Display"));
    screen = ui_label_create(win, "-");
    ui_layout_add(box, screen);
    root = box;
}

void SystemPage::refresh() {
    if (!root) return;
    ui_label_set_text(ram, formatRam().c_str());
    ui_label_set_text(cpu, formatCpu().c_str());
    ui_label_set_text(gpu, formatGpu().c_str());
    ui_label_set_text(screen, formatScreen().c_str());
}

}  // namespace settings
