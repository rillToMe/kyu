// apps/taskmgr/task_manager.cpp — TaskManagerApp: window, sidebar, tick, aksi.
#include "task_manager.hpp"

#include "format.hpp"

namespace taskmgr {

namespace {

// Refresh ~500 ms (pola taskmgr.c lama).
constexpr std::uint64_t kTickMs = 500;
constexpr int kWinW = 720;
constexpr int kWinH = 520;
constexpr int kSidebarW = 140;
constexpr int kGap = 8;

// Sidebar digenerate dari satu koleksi (satu callback generik, pola settings).
const NavigationItem kNav[] = {
    {TaskPage::Processes, "Processes"},
    {TaskPage::Performance, "Performance"},
    {TaskPage::Memory, "Memory"},
};
constexpr int kNavCount = 3;

void onSidebar(void* ud) { TaskManagerApp::self(ud)->onSidebarChanged(); }
void onEndTaskThunk(void* ud) { TaskManagerApp::self(ud)->onEndTask(); }
void onRefreshThunk(void* ud) { TaskManagerApp::self(ud)->onRefresh(); }
int onTickThunk(void* ud) { return TaskManagerApp::self(ud)->onTick(); }

void onKeyThunk(void* ud, std::uint32_t ascii, std::uint32_t scancode,
                std::uint32_t mods) {
    (void)scancode;
    (void)mods;
    TaskManagerApp::self(ud)->onKey(ascii);
}

}  // namespace

TaskManagerApp::TaskManagerApp()
    : win_(nullptr),
      sidebar_(nullptr),
      current_(TaskPage::Processes),
      last_tick_ms_(0) {}

TaskManagerApp::~TaskManagerApp() {
    if (win_) ui_window_destroy(win_);
}

int TaskManagerApp::run() {
    int ww = kWinW, hh = kWinH;
    std::uint32_t sw = 0, sh = 0;
    if (sys_get_screen_size(&sw, &sh) == 0 && sw > 0 && sh > 0) {
        if ((int)sw < ww) ww = (int)sw > 40 ? (int)sw - 40 : (int)sw;
        if ((int)sh < hh) hh = (int)sh > 60 ? (int)sh - 60 : (int)sh;
    }
    win_ = ui_window_create((std::uint32_t)ww, (std::uint32_t)hh);
    if (!win_) return 1;
    ui_window_set_title(win_, "Task Manager");
    ui_settings_load(win_);
    ui_window_set_key(win_, onKeyThunk, this);
    ui_window_set_tick(win_, onTickThunk, this);

    buildUi();
    showPage(TaskPage::Processes);
    refreshAll();

    ui_window_run(win_);  // blocking; keluar via X / ESC
    return 0;
}

void TaskManagerApp::buildUi() {
    const int content_h = 520 - 110;
    const int content_w = 720 - kSidebarW - kGap * 3;

    ui_widget_t* root = ui_hbox_create(win_, kGap);

    sidebar_ = ui_listview_create(win_, kSidebarW, content_h);
    for (int i = 0; i < kNavCount; i++)
        ui_listview_add_item(sidebar_, kNav[i].title);
    ui_listview_set_change(sidebar_, onSidebar, this);
    ui_layout_add(root, sidebar_);

    // Satu ScrollView menampung semua page; page non-aktif disembunyikan
    // (layout melewati anak tersembunyi — pola settings).
    ui_widget_t* scroll = ui_scrollview_create(win_, content_w, content_h);
    ui_widget_t* pages = ui_vbox_create(win_, 8);

    processes_.build(win_);
    performance_.build(win_);
    memory_.build(win_);

    ui_layout_add(pages, processes_.root);
    ui_layout_add(pages, performance_.root);
    ui_layout_add(pages, memory_.root);

    ui_scrollview_set_child(scroll, pages);
    ui_layout_add(root, scroll);

    // Bar aksi milik page Processes (End task / Refresh): dibuat sekali di
    // sini, callback ke app (butuh model). Ditaruh di bawah scrollview? Tidak
    // — libui root tunggal: bungkus root + action bar dalam vbox luar.
    ui_widget_t* outer = ui_vbox_create(win_, 6);
    ui_layout_add(outer, root);

    ui_widget_t* btnrow = ui_hbox_create(win_, 6);
    ui_widget_t* bend = ui_button_create(win_, "End task");
    ui_button_set_click(bend, onEndTaskThunk, this);
    ui_layout_add(btnrow, bend);
    ui_widget_t* bref = ui_button_create(win_, "Refresh");
    ui_button_set_click(bref, onRefreshThunk, this);
    ui_layout_add(btnrow, bref);
    ui_layout_add(outer, btnrow);

    ui_window_add(win_, outer);
}

void TaskManagerApp::showPage(TaskPage page) {
    current_ = page;
    const int idx = static_cast<int>(page);
    ui_widget_t* roots[kNavCount] = {
        processes_.root, performance_.root, memory_.root,
    };
    for (int i = 0; i < kNavCount; i++)
        ui_widget_set_visible(roots[i], i == idx);
    ui_listview_set_selected(sidebar_, idx);
}

void TaskManagerApp::onSidebarChanged() {
    const int sel = ui_listview_selected(sidebar_);
    if (sel < 0 || sel >= kNavCount) return;
    showPage(kNav[sel].page);
}

// Keyboard: 1/2/3 = pindah page, R = refresh, S = ganti kunci sort,
// E = end task baris terpilih. (Pola settings: shortcut tanpa mouse,
// memungkinkan verifikasi headless via sendkey QEMU.)
void TaskManagerApp::onKey(std::uint32_t ascii) {
    if (ascii == '1') showPage(TaskPage::Processes);
    else if (ascii == '2') showPage(TaskPage::Performance);
    else if (ascii == '3') showPage(TaskPage::Memory);
    else if (ascii == 'r' || ascii == 'R') onRefresh();
    else if (ascii == 'e' || ascii == 'E') onEndTask();
    else if (ascii == 's' || ascii == 'S') onCycleSort();
}

void TaskManagerApp::onRefresh() {
    refreshAll();
}

void TaskManagerApp::onCycleSort() {
    const SortKey cur = procs_.sortKey();
    SortKey next = SortKey::Pid;
    if (cur == SortKey::Pid) next = SortKey::Name;
    else if (cur == SortKey::Name) next = SortKey::State;
    procs_.sort(next);
    showPage(TaskPage::Processes);
    refreshAll();
}

void TaskManagerApp::onEndTask() {
    const std::int32_t pid = processes_.selectedPid(procs_);
    if (pid < 0) {
        setStatus("Select a process row first");
        return;
    }
    // Kill SELALU lewat syscall kernel (otorisasi di kernel: parent/root/
    // self-guard app). GUI tidak pernah memutasi state task langsung.
    const KillResult r = procs_.kill(pid);
    if (r == KillResult::Killed) {
        setStatusStr(std::string("Kill PID ") + toStr32(pid) + " OK");
        procs_.refresh();
        processes_.refresh(procs_);
    } else if (r == KillResult::SelfDenied) {
        setStatus("Refused: task manager itself");
    } else {
        // ABI sys_kill hanya 0/-1: alasan pasti (ditolak/sudah keluar/PID tak
        // valid) tidak bisa dibedakan — dilaporkan apa adanya.
        setStatusStr(std::string("Kill PID ") + toStr32(pid) +
                     " failed/denied");
    }
}

int TaskManagerApp::onTick() {
    const std::uint64_t now = sys_uptime();
    if (now - last_tick_ms_ < kTickMs) return 0;
    last_tick_ms_ = now;
    refreshAll();
    return 1;
}

void TaskManagerApp::refreshAll() {
    sys_.refresh();
    const bool ok = procs_.refresh();
    performance_.refresh(sys_.stats());
    memory_.refresh(sys_.stats());
    processes_.refresh(procs_);
    if (!ok) setStatus("proc_list failed");
}

void TaskManagerApp::setStatus(const char* text) {
    ui_label_set_text(processes_.status, text);
}

void TaskManagerApp::setStatusStr(const std::string& text) {
    ui_label_set_text(processes_.status, text.c_str());
}

}  // namespace taskmgr
