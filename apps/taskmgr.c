// ============================================================
// taskmgr.c — Task Manager (Phase 10 + P0 Phase 3): monitor
// RAM/CPU/Disk (libui) + daftar proses + aksi Kill via sys_kill.
//
// Kill SELALU lewat syscall kernel (otorisasi di kernel: parent/root).
// GUI tidak pernah memutasi state task langsung. Hasil kill
// (sukses / ditolak / tak valid / sudah keluar) tampil di status.
// ============================================================
#include "userlib.h"
#include "libui.h"

static ui_widget_t* g_ram;   static ui_widget_t* g_rambar;
static ui_widget_t* g_cpu;   static ui_widget_t* g_cpubar;
static ui_widget_t* g_disk;  static ui_widget_t* g_diskbar;
static ui_widget_t* g_up;
static ui_widget_t* g_table;
static ui_widget_t* g_status;
static uint64_t last_refresh = 0;

// PID per baris tabel (urutan = urutan sys_proc_list terakhir).
static int32_t g_pids[16];
static int g_nprocs = 0;

// Nama state TASK_* (task.h) tanpa include kernel-internal.
static const char* state_name(uint8_t st) {
    switch (st) {
        case 0: return "READY";
        case 1: return "RUNNING";
        case 2: return "SLEEP";
        case 3: return "DEAD";
        case 4: return "BLOCK";
        case 5: return "ZOMBIE";
        default: return "?";
    }
}

static void itoa(uint32_t n, char* b) {
    if (n == 0) { b[0] = '0'; b[1] = '\0'; return; }
    char t[16]; int i = 0;
    while (n > 0 && i < 15) { t[i++] = '0' + (n % 10); n /= 10; }
    int j = 0; while (i > 0) b[j++] = t[--i]; b[j] = '\0';
}

static void itoa_i32(int32_t n, char* b) {
    if (n < 0) { b[0] = '-'; itoa((uint32_t)(-(int64_t)n), b + 1); }
    else itoa((uint32_t)n, b);
}

// "RAM : 42 MB / 1024 MB"
static void label_ratio(ui_widget_t* lbl, const char* pre, uint32_t used,
                        uint32_t tot, const char* suf) {
    char b[48]; int k = 0;
    for (int i = 0; pre[i] && k < 20; i++) b[k++] = pre[i];
    char n[16]; itoa(used, n);
    for (int i = 0; n[i] && k < 30; i++) b[k++] = n[i];
    const char* sep = " MB / ";
    for (int i = 0; sep[i] && k < 38; i++) b[k++] = sep[i];
    itoa(tot, n);
    for (int i = 0; n[i] && k < 44; i++) b[k++] = n[i];
    for (int i = 0; suf[i] && k < 47; i++) b[k++] = suf[i];
    b[k] = '\0';
    ui_label_set_text(lbl, b);
}

static void set_status(const char* text) {
    ui_label_set_text(g_status, text);
}

// Muat ulang tabel proses dari kernel (read-only snapshot).
static void refresh_procs(void) {
    proc_info_t list[16];
    int n = sys_proc_list(list, 16);
    if (n < 0) { set_status("proc_list gagal"); return; }
    ui_table_clear(g_table);
    g_nprocs = 0;
    for (int i = 0; i < n && i < 16; i++) {
        char c_pid[12], c_name[20], c_uid[12], c_state[24];
        itoa_i32(list[i].pid, c_pid);
        int k = 0;
        while (k < 15 && list[i].name[k]) { c_name[k] = list[i].name[k]; k++; }
        c_name[k] = '\0';
        itoa(list[i].uid, c_uid);
        // "ZOMBIE(125)": alasan keluar terlihat langsung di daftar.
        const char* sn = state_name(list[i].state);
        k = 0;
        while (sn[k] && k < 10) { c_state[k] = sn[k]; k++; }
        if (list[i].state == 5) {
            char code[12]; itoa_i32(list[i].exit_code, code);
            if (k < 20) c_state[k++] = '(';
            for (int j = 0; code[j] && k < 22; j++) c_state[k++] = code[j];
            if (k < 23) c_state[k++] = ')';
        }
        c_state[k] = '\0';
        const char* row[4];
        row[0] = c_pid; row[1] = c_name; row[2] = c_uid; row[3] = c_state;
        ui_table_add_row(g_table, row, 4);
        g_pids[g_nprocs++] = list[i].pid;
    }
}

static void on_refresh(void* userdata) {
    (void)userdata;
    refresh_procs();
    char b[40]; int k = 0;
    const char* pre = "Proses: ";
    while (pre[k]) { b[k] = pre[k]; k++; }
    char n[8]; itoa((uint32_t)g_nprocs, n);
    int j = 0; while (n[j] && k < 38) b[k++] = n[j++];
    b[k] = '\0';
    set_status(b);
}

static void on_kill(void* userdata) {
    (void)userdata;
    int sel = ui_table_selected(g_table);
    if (sel < 0 || sel >= g_nprocs) { set_status("Pilih baris proses dulu"); return; }
    int32_t pid = g_pids[sel];
    if (pid == sys_getpid()) { set_status("Tolak: task manager sendiri"); return; }
    int r = sys_kill(pid);
    char b[48]; int k = 0;
    if (r == 0) {
        const char* pre = "Kill PID ";
        while (pre[k]) { b[k] = pre[k]; k++; }
        char n[12]; itoa_i32(pid, n);
        int j = 0; while (n[j] && k < 30) b[k++] = n[j++];
        const char* suf = " OK";
        j = 0; while (suf[j] && k < 46) b[k++] = suf[j++];
        b[k] = '\0';
        set_status(b);
        refresh_procs();   // zombie hilang setelah parent reap; daftar segar
    } else {
        const char* pre = "Kill PID ";
        while (pre[k]) { b[k] = pre[k]; k++; }
        char n[12]; itoa_i32(pid, n);
        int j = 0; while (n[j] && k < 24) b[k++] = n[j++];
        const char* suf = " ditolak/sudah keluar";
        j = 0; while (suf[j] && k < 46) b[k++] = suf[j++];
        b[k] = '\0';
        set_status(b);
    }
}

static int tick(void* userdata) {
    (void)userdata;
    uint64_t now = sys_uptime();
    if (now - last_refresh < 500) return 0;
    last_refresh = now;

    uint32_t tram = sys_total_ram() / 1024 / 1024;
    uint32_t uram = sys_used_ram()  / 1024 / 1024;
    if (!tram) tram = 1;
    label_ratio(g_ram, "RAM : ", uram, tram, " MB");
    ui_progressbar_set_value(g_rambar, uram * 100 / tram);

    uint32_t cpu = sys_get_cpu_usage();
    label_ratio(g_cpu, "CPU : ", cpu, 100, " %");
    ui_progressbar_set_value(g_cpubar, cpu);

    uint32_t tdisk = sys_get_total_disk() / 1024 / 1024;
    uint32_t udisk = sys_get_used_disk()  / 1024 / 1024;
    if (!tdisk) tdisk = 1;
    label_ratio(g_disk, "Disk: ", udisk, tdisk, " MB");
    ui_progressbar_set_value(g_diskbar, udisk * 100 / tdisk);

    label_ratio(g_up, "Uptime: ", (uint32_t)(now / 1000), 0, " s");
    return 1;
}

void main(void) {
    ui_window_t* win = ui_window_create(480, 440);
    if (!win) { sys_exit(); }
    ui_window_set_title(win, "Task Manager");

    ui_widget_t* box = ui_vbox_create(win, 6);
    g_ram = ui_label_create(win, "RAM : -");
    ui_layout_add(box, g_ram);
    g_rambar = ui_progressbar_create(win, 440);
    ui_layout_add(box, g_rambar);
    g_cpu = ui_label_create(win, "CPU : -");
    ui_layout_add(box, g_cpu);
    g_cpubar = ui_progressbar_create(win, 440);
    ui_layout_add(box, g_cpubar);
    g_disk = ui_label_create(win, "Disk: -");
    ui_layout_add(box, g_disk);
    g_diskbar = ui_progressbar_create(win, 440);
    ui_layout_add(box, g_diskbar);
    g_up = ui_label_create(win, "Uptime: -");
    ui_layout_add(box, g_up);

    // P0 Phase 3: daftar proses (PID/Nama/UID/State) + Kill.
    ui_layout_add(box, ui_label_create(win, "Proses:"));
    g_table = ui_table_create(win, 440, 150);
    ui_table_add_column(g_table, "PID", 60);
    ui_table_add_column(g_table, "Nama", 180);
    ui_table_add_column(g_table, "UID", 70);
    ui_table_add_column(g_table, "State", 120);
    ui_layout_add(box, g_table);

    ui_widget_t* btnrow = ui_hbox_create(win, 6);
    ui_widget_t* bkill = ui_button_create(win, "Kill");
    ui_button_set_click(bkill, on_kill, 0);
    ui_layout_add(btnrow, bkill);
    ui_widget_t* bref = ui_button_create(win, "Refresh");
    ui_button_set_click(bref, on_refresh, 0);
    ui_layout_add(btnrow, bref);
    ui_layout_add(box, btnrow);

    g_status = ui_label_create(win, "Pilih proses, lalu Kill");
    ui_layout_add(box, g_status);

    ui_window_add(win, box);

    ui_window_set_tick(win, tick, 0);
    tick(0);   // render nilai awal
    refresh_procs();
    ui_window_run(win);   // blocking; keluar via X / ESC
    ui_window_destroy(win);
    sys_exit();
}
