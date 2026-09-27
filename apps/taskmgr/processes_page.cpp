// apps/taskmgr/processes_page.cpp — implementasi halaman Processes.
#include "processes_page.hpp"

#include "format.hpp"

namespace taskmgr {

const char* sortName(SortKey key) {
    switch (key) {
        case SortKey::Name: return "Name";
        case SortKey::State: return "State";
        case SortKey::Pid:
        default: return "PID";
    }
}

void ProcessesPage::build(ui_window_t* win) {
    ui_widget_t* box = ui_vbox_create(win, 6);
    ui_layout_add(box, ui_label_create(win, "Processes"));
    table = ui_table_create(win, 520, 300);
    ui_table_add_column(table, "PID", 60);
    ui_table_add_column(table, "Name", 200);
    ui_table_add_column(table, "UID", 70);
    ui_table_add_column(table, "State", 150);
    ui_table_set_empty_text(table, "No processes");
    ui_layout_add(box, table);
    status = ui_label_create(win, "Select a process, then End task");
    ui_layout_add(box, status);
    root = box;
}

void ProcessesPage::refresh(const ProcessModel& model) {
    if (!root) return;
    // Pertahankan seleksi per-PID supaya refresh 500 ms tidak menggeser fokus.
    const std::int32_t keep = selectedPid(model);
    ui_table_clear(table);
    const std::vector<Process>& procs = model.processes();
    for (int i = 0; i < (int)procs.size(); i++) {
        const Process& p = procs[i];
        const std::string s_pid = toStr32(p.pid);
        const std::string s_uid = toStr((unsigned long)p.uid);
        const std::string s_state = stateLabel(p);
        const char* row[4];
        row[0] = s_pid.c_str();
        row[1] = p.name.c_str();
        row[2] = s_uid.c_str();
        row[3] = s_state.c_str();
        // ui_table_add_row menyalin teks (kontrak libui: text di-copy) —
        // string sementara aman.
        ui_table_add_row(table, row, 4);
    }
    if (keep >= 0) {
        const int idx = model.findByPid(keep);
        ui_table_set_selected(table, idx);
    }
    const std::string st = toStr((unsigned long)model.count()) +
                           " processes | Sort: " + sortName(model.sortKey());
    ui_label_set_text(status, st.c_str());
}

std::int32_t ProcessesPage::selectedPid(const ProcessModel& model) const {
    if (!table) return -1;
    const int sel = ui_table_selected(table);
    const std::vector<Process>& procs = model.processes();
    if (sel < 0 || sel >= (int)procs.size()) return -1;
    return procs[sel].pid;
}

}  // namespace taskmgr
