// apps/taskmgr/memory_page.cpp — implementasi halaman Memory.
#include "memory_page.hpp"

#include "format.hpp"

namespace taskmgr {

void MemoryPage::build(ui_window_t* win) {
    ui_widget_t* box = ui_vbox_create(win, 6);
    ui_layout_add(box, ui_label_create(win, "Memory"));
    summary = ui_label_create(win, "-");
    ui_layout_add(box, summary);
    bar = ui_progressbar_create(win, 500);
    ui_layout_add(box, bar);
    used = ui_label_create(win, "-");
    ui_layout_add(box, used);
    free = ui_label_create(win, "-");
    ui_layout_add(box, free);
    total = ui_label_create(win, "-");
    ui_layout_add(box, total);
    // Batas ABI, dinyatakan — bukan angka:
    // kernel hanya mengekspos total+terpakai (sys_total_ram/sys_used_ram).
    ui_layout_add(box, ui_label_create(win, "Breakdown:"));
    ui_layout_add(box, ui_label_create(
                           win, "Per-category use (kernel, processes, page"));
    ui_layout_add(box, ui_label_create(
                           win, "tables, cache) is not exposed by the"));
    ui_layout_add(box, ui_label_create(win, "kernel: only total/used shown."));
    root = box;
}

void MemoryPage::refresh(const SystemStats& st) {
    if (!root) return;
    const MemoryStats& m = st.memory;
    const std::string s = mbStr(m.used) + " / " + mbStr(m.total) + " (" +
                          toStr((unsigned long)m.percent()) + "%)";
    ui_label_set_text(summary, s.c_str());
    ui_progressbar_set_value(bar, m.percent());
    const std::string u = std::string("Used:  ") + mbStr(m.used);
    ui_label_set_text(used, u.c_str());
    const std::string f = std::string("Free:  ") + mbStr(m.free);
    ui_label_set_text(free, f.c_str());
    const std::string t = std::string("Total: ") + mbStr(m.total);
    ui_label_set_text(total, t.c_str());
}

}  // namespace taskmgr
