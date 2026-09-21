// ============================================================
// fileman.c — Explorer (Phase 10): File Manager KyuzenFS (libui).
// Table nama+ukuran; klik pilih; Buka/Refresh. .elf → spawn,
// .png → view.tmp + viewer.elf, .txt → edit.tmp + notepad.elf.
//
// Tema: dark mode monokrom gaya Windows Explorer — latar #202020,
// aksen abu-abu; TANPA biru (header/seleksi/hover memakai abu netral).
// Folder selalu di urutan atas, ukuran ditampilkan human-readable
// (B/KB/MB, satu desimal) seperti Explorer.
//
// Build: fileman.o + userlib.o + libgui.o + libui.o + png.o
// ============================================================
#include "userlib.h"
#include "libui.h"
#include "color_types.h"

#define MAX_FILES 16
#define WIN_W 480
#define WIN_H 360
#define INNER_W (WIN_W - 16)      // root VBox bermargin 8px kiri/kanan
#define BTN_H 28
#define STATUS_H 20

static ui_window_t* g_win;
static ui_widget_t* g_table;
static ui_widget_t* g_status;
static file_info_t g_files[MAX_FILES];
static int g_nfiles = 0;

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

// --- ukuran human-readable gaya Explorer: "512 B", "12.3 KB", "1.4 MB".
// Semua integer (build -msoft-float): desimal lewat fixed-point.
static void format_size(uint32_t n, char* b) {
    uint32_t whole, tenth;
    if (n < 1024) {
        int i = 0; if (n == 0) b[i++] = '0';
        char t[12]; int k = 0;
        uint32_t v = n;
        while (v > 0 && k < 11) { t[k++] = '0' + (v % 10); v /= 10; }
        while (k > 0) b[i++] = t[--k];
        b[i++] = ' '; b[i++] = 'B'; b[i] = '\0';
        return;
    }
    const char* unit;
    if (n < 1024u * 1024) {
        whole = n >> 10;
        tenth = ((n & 1023u) * 10u) >> 10;      // < 10240, aman di 32-bit
        unit = "KB";
    } else {
        whole = n >> 20;
        tenth = ((n & 0xFFFFFu) * 10u) >> 20;   // < 10485760, aman di 32-bit
        unit = "MB";
    }
    int i = 0; char t[12]; int k = 0;
    while (whole > 0 && k < 11) { t[k++] = '0' + (int)(whole % 10); whole /= 10; }
    while (k > 0) b[i++] = t[--k];
    b[i++] = '.'; b[i++] = '0' + (int)tenth;
    for (int u = 0; unit[u]; u++) b[i++] = unit[u];
    b[i] = '\0';
}

// --- peserta daftar: folder dulu, lalu nama A-Z (gaya Explorer).
static int name_less(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return (uint8_t)*a < (uint8_t)*b; a++; b++; }
    return *a == '\0' && *b != '\0';
}
static void sort_entries(void) {
    for (int i = 1; i < g_nfiles; i++) {
        file_info_t key = g_files[i];
        int j = i - 1;
        while (j >= 0 &&
               (g_files[j].is_folder < key.is_folder ||
                (g_files[j].is_folder == key.is_folder &&
                 name_less(key.filename, g_files[j].filename)))) {
            g_files[j + 1] = g_files[j];
            j--;
        }
        g_files[j + 1] = key;
    }
}

// Status bar: kiri jumlah item, kanan nama yang dipilih (gaya Explorer).
static void update_status(int sel) {
    char left[32]; int i = 0;
    char t[12]; int k = 0;
    uint32_t v = (uint32_t)g_nfiles;
    if (v == 0) left[i++] = '0';
    while (v > 0 && k < 11) { t[k++] = '0' + (char)(v % 10); v /= 10; }
    while (k > 0) left[i++] = t[--k];
    left[i++] = ' '; left[i++] = 'i'; left[i++] = 't'; left[i++] = 'e'; left[i++] = 'm';
    left[i] = '\0';

    const char* right = (sel >= 0 && sel < g_nfiles) ? g_files[sel].filename : "";
    ui_statusbar_set_text(g_status, left, right);
}

static void fill_table(void) {
    ui_table_clear(g_table);
    g_nfiles = sys_get_file_list("/", g_files, MAX_FILES);
    if (g_nfiles < 0) g_nfiles = 0;
    sort_entries();
    for (int i = 0; i < g_nfiles; i++) {
        char sz[16];
        if (g_files[i].is_folder) { sz[0] = '\0'; }   // folder: kolom ukuran kosong
        else format_size(g_files[i].size, sz);
        const char* cells[2] = { g_files[i].filename, sz };
        ui_table_add_row(g_table, cells, 2);
    }
    update_status(-1);
}

static void on_select(void* userdata) {
    (void)userdata;
    update_status(ui_table_selected(g_table));
}

static void do_open(void* userdata) {
    (void)userdata;
    int sel = ui_table_selected(g_table);
    if (sel < 0 || sel >= g_nfiles) return;
    char* fname = g_files[sel].filename;
    int len = slen(fname);

    if (len > 4 && fname[len-4]=='.' && fname[len-3]=='e' &&
        fname[len-2]=='l' && fname[len-1]=='f') {
        char p[32];
        build_app_path(p, sizeof(p), fname);
        ui_window_destroy(g_win);
        sys_exec(p);            // tak pernah kembali
    } else if (len > 4 && fname[len-4]=='.' && fname[len-3]=='p' &&
               fname[len-2]=='n' && fname[len-1]=='g') {
        if (sys_file_exists("view.tmp")) fs_delete("view.tmp");
        sys_create_file("view.tmp", fname, len);   // arg ke viewer
        char vp[32];
        build_app_path(vp, sizeof(vp), "viewer.elf");
        ui_window_destroy(g_win);
        sys_exec(vp);
    } else if (len > 4 && fname[len-4]=='.' && fname[len-3]=='t' &&
               fname[len-2]=='x' && fname[len-1]=='t') {
        if (sys_file_exists("edit.tmp")) fs_delete("edit.tmp");
        sys_create_file("edit.tmp", fname, len);   // arg ke notepad
        char np[32];
        build_app_path(np, sizeof(np), "notepad.elf");
        ui_window_destroy(g_win);
        sys_exec(np);
    }
}

static void do_refresh(void* userdata) { (void)userdata; fill_table(); }

void main(void) {
    g_win = ui_window_create(WIN_W, WIN_H);
    if (!g_win) { sys_exit(); }
    ui_window_set_title(g_win, "Explorer");

    // Tema dark monokrom (Windows Explorer dark): permukaan #202020,
    // teks #CCCCCC, header/seleksi/tombol abu netral — TANPA biru.
    ui_theme_t theme;
    theme.bg           = COLOR_RGB(0x20, 0x20, 0x20);
    theme.fg           = COLOR_RGB(0xCC, 0xCC, 0xCC);
    theme.accent       = COLOR_RGB(0x9A, 0x9A, 0x9A);   // netral, bukan biru
    theme.button_bg    = COLOR_RGB(0x3A, 0x3A, 0x3A);   // header + tombol + seleksi
    theme.button_fg    = COLOR_RGB(0xE6, 0xE6, 0xE6);
    theme.button_hover = COLOR_RGB(0x45, 0x45, 0x45);
    ui_window_set_theme(g_win, &theme);

    // --- body: daftar berkas ---
    ui_widget_t* box = ui_vbox_create(g_win, 6);
    g_table = ui_table_create(g_win, INNER_W, WIN_H - 16 - 12 - BTN_H - STATUS_H);
    ui_table_add_column(g_table, "Nama", INNER_W - 108);
    ui_table_add_column(g_table, "Ukuran", 108);
    ui_table_set_change(g_table, on_select, 0);
    ui_layout_add(box, g_table);

    // --- baris aksi ---
    ui_widget_t* btns = ui_hbox_create(g_win, 8);
    ui_widget_t* b = ui_button_create(g_win, "Buka");
    ui_button_set_click(b, do_open, 0);
    ui_layout_add(btns, b);
    b = ui_button_create(g_win, "Refresh");
    ui_button_set_click(b, do_refresh, 0);
    ui_layout_add(btns, b);
    ui_layout_add(box, btns);

    // --- status bar: "8 item" | nama terpilih ---
    g_status = ui_statusbar_create(g_win);
    ui_widget_set_size(g_status, INNER_W, STATUS_H);
    ui_layout_add(box, g_status);

    ui_window_add(g_win, box);
    fill_table();

    ui_window_run(g_win);   // blocking; keluar via X / ESC
    ui_window_destroy(g_win);
    sys_exit();
}
