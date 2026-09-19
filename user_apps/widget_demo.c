// user_apps/widget_demo.c — demo Toolkit libui dari sisi C ABI.
//
// Phase 8: showcase Advanced Widgets — MenuBar, Toolbar, Tab, ListView,
// Table, TreeView, ScrollView — plus regresi Phase 7 (Kontrol) & Phase 6.
// Semua lewat API C murni, tanpa C++ bocor.
//
// Layout: MenuBar + Toolbar (add_bar) di puncak, label status, lalu Tab
// (Kontrol/Daftar/Tabel/Pohon/Gulir). Scroll roda + scrollbar di daftar/
// tabel/pohon/gulir; menu dropdown via popup.
//
// Build: widget_demo.o + userlib.o + libgui.o + libui.o + png.o

#include "userlib.h"
#include "libui.h"
#include "color_utils.h"   // palet + COLOR_RGB_INIT/COLOR_WHITE_INIT (libs/color)

static ui_widget_t* status;     // label status (di atas tab)
static ui_widget_t* lbl;        // counter "+1" (regresi Phase 6)
static ui_widget_t* teks_lbl;   // echo isi TextBox saat Enter
static ui_widget_t* tb;
static ui_widget_t* slider;
static ui_widget_t* progress;
static ui_widget_t* lv;         // ListView (panel Daftar)
static ui_widget_t* tabel;      // Table (panel Tabel)
static ui_widget_t* pohon;      // TreeView (panel Pohon)
static int count = 0;

static void itoa(int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16]; int i = 0;
    while (n > 0 && i < 15) { tmp[i++] = '0' + (n % 10); n /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

// Salin pre diikuti itoa(n) ke buf; buf cukup 32 byte.
static void cat_num(char* buf, const char* pre, int n) {
    int k = 0;
    while (pre[k]) { buf[k] = pre[k]; k++; }
    itoa(n, buf + k);
}

static void set_status(const char* s) { if (status) ui_label_set_text(status, s); }

// Callback bersama untuk item menu & tombol toolbar: userdata = teks status.
static void on_menu(void* userdata) { set_status((const char*)userdata); }

// --- regresi Phase 6: counter ---
static void on_plus(void* userdata) {
    (void)userdata;
    count++;
    char buf[24];
    cat_num(buf, "Klik: ", count);
    ui_label_set_text(lbl, buf);
}

// --- regresi Phase 7 ---
static void on_enter(void* userdata) {
    (void)userdata;
    const char* t = ui_textbox_text(tb);
    char buf[72];
    int k = 0;
    while (k < 70 && t[k]) { buf[k] = t[k]; k++; }
    buf[k] = '\0';
    const char* pre = "Teks: ";
    char out[80];
    k = 0;
    while (pre[k]) { out[k] = pre[k]; k++; }
    int i = 0;
    while (buf[i] && k < 78) out[k++] = buf[i++];
    out[k] = '\0';
    ui_label_set_text(teks_lbl, out);
}

static void on_slider(void* userdata) {
    (void)userdata;
    ui_progressbar_set_value(progress, ui_slider_value(slider));
}

// --- Phase 8: selection di daftar/tabel/pohon ---
static void on_list(void* userdata) {
    (void)userdata;
    int sel = ui_listview_selected(lv);
    if (sel < 0) return;
    char buf[32];
    cat_num(buf, "Daftar: Item ", sel + 1);
    set_status(buf);
}

static void on_table(void* userdata) {
    (void)userdata;
    int sel = ui_table_selected(tabel);
    if (sel < 0) return;
    char buf[32];
    cat_num(buf, "Tabel: baris ", sel + 1);
    set_status(buf);
}

static void on_tree(void* userdata) {
    (void)userdata;
    int sel = ui_treeview_selected(pohon);
    if (sel < 0) return;
    char buf[32];
    cat_num(buf, "Pohon: node ", sel + 1);
    set_status(buf);
}

// --- Phase 9: Desktop Services ---
static ui_window_t* g_win;

static void on_dialog(void* userdata, int index) {
    (void)userdata;
    const char* s;
    if (index == 0) s = "Dialog: Ya";
    else if (index == 1) s = "Dialog: Tidak";
    else s = "Dialog: Batal (ESC)";
    set_status(s);
}
static void on_dialog_btn(void* userdata) {
    (void)userdata;
    const char* b[2] = { "Ya", "Tidak" };
    ui_dialog_show(g_win, "Konfirmasi", "Hapus file?", b, 2, on_dialog, 0);
}
static void on_shortcut_new(void* userdata) {
    (void)userdata;
    const char* b[1] = { "OK" };
    ui_dialog_show(g_win, "Shortcut", "Ctrl+N ditekan", b, 1, on_dialog, 0);
}
static void on_notif_btn(void* userdata) {
    (void)userdata;
    ui_window_notify(g_win, "Halo dari KyuzenOS", 3000);
    set_status("Notifikasi: 3 detik");
}

// Preset tema — disalin ke window oleh ui_window_set_theme (bukan pointer).
static const ui_theme_t tema_gelap = {
    COLOR_RGB_INIT(0x12, 0x12, 0x12), COLOR_RGB_INIT(0xE0, 0xE0, 0xE0),
    COLOR_RGB_INIT(0xE9, 0x45, 0x60), COLOR_RGB_INIT(0x0F, 0x34, 0x60),
    COLOR_WHITE_INIT,          COLOR_RGB_INIT(0x2A, 0x4A, 0x7E),
};
static const ui_theme_t tema_terang = {
    COLOR_RGB_INIT(0xF0, 0xF0, 0xF0), COLOR_RGB_INIT(0x22, 0x22, 0x22),
    COLOR_RGB_INIT(0xD3, 0x2F, 0x2F), COLOR_RGB_INIT(0xCF, 0xD8, 0xDC),
    COLOR_RGB_INIT(0x22, 0x22, 0x22), COLOR_RGB_INIT(0x90, 0xA4, 0xAE),
};
static const ui_theme_t tema_hijau = {
    COLOR_RGB_INIT(0x0D, 0x1F, 0x14), COLOR_RGB_INIT(0xDF, 0xF2, 0xE0),
    COLOR_RGB_INIT(0x4C, 0xAF, 0x50), COLOR_RGB_INIT(0x1B, 0x4D, 0x2E),
    COLOR_RGB_INIT(0xE8, 0xF5, 0xE9), COLOR_RGB_INIT(0x2E, 0x7D, 0x46),
};

static void on_theme(void* userdata) {
    ui_window_set_theme(g_win, (const ui_theme_t*)userdata);
    set_status("Tema: diubah");
}
static void on_settings_save(void* userdata) {
    (void)userdata;
    set_status(ui_settings_save(g_win) ? "Setelan: disimpan" : "Setelan: gagal");
}
static void on_settings_load(void* userdata) {
    (void)userdata;
    set_status(ui_settings_load(g_win) ? "Setelan: dimuat" : "Setelan: tak ada/batal");
}
static void on_clip_copy(void* userdata) {
    (void)userdata;
    ui_clipboard_set_text(ui_textbox_text(tb));
    set_status("Clipboard: disalin");
}
static void on_clip_paste(void* userdata) {
    (void)userdata;
    ui_textbox_set_text(tb, ui_clipboard_get_text());
    set_status("Clipboard: ditempel");
}
static void on_drop(void* userdata, const char* payload, int x, int y) {
    (void)userdata; (void)x; (void)y;
    if (!payload) { set_status("Drop: kosong"); return; }
    char out[48];
    const char* pre = "Drop: ";
    int j = 0;
    while (pre[j]) { out[j] = pre[j]; j++; }
    for (int i = 0; payload[i] && j < 46; i++) out[j++] = payload[i];
    out[j] = '\0';
    set_status(out);
}

static const struct { const char* nama, *status_, *nilai; } row_data[] = {
    { "Kernel",     "OK",   "100" },
    { "VFS",        "OK",   "95"  },
    { "LwIP",       "OK",   "88"  },
    { "e1000",      "OK",   "92"  },
    { "ATA",        "OK",   "90"  },
    { "USB",        "FAIL", "12"  },
    { "Audio",      "N/A",  "-"   },
    { "KWM",        "OK",   "98"  },
    { "libgui",     "OK",   "96"  },
    { "libui",      "OK",   "97"  },
    { "Scheduler",  "OK",   "99"  },
    { "Framebuf",   "OK",   "94"  },
    { "Syscall",    "OK",   "93"  },
};

static const struct { const char* label; int depth; int expanded; } tree_data[] = {
    { "Sistem",      0, 1 },
    { "Inti",        1, 1 },
    { "Scheduler",   2, 1 },
    { "Memori",      2, 1 },
    { "Perangkat",   1, 0 },
    { "ATA",         2, 1 },
    { "e1000",       2, 1 },
    { "Pengguna",    0, 1 },
    { "Dokumen",     1, 1 },
    { "Laporan.txt", 2, 1 },
};

void main(void) {
    ui_window_t* win = ui_window_create(360, 400);
    if (!win) { sys_exit(); }
    g_win = win;

    ui_theme_t th;
    th.bg           = COLOR_RGB(0x12, 0x12, 0x12);
    th.fg           = COLOR_RGB(0xE0, 0xE0, 0xE0);
    th.accent       = COLOR_RGB(0xE9, 0x45, 0x60);
    th.button_bg    = COLOR_RGB(0x0F, 0x34, 0x60);
    th.button_fg    = COLOR_WHITE;
    th.button_hover = COLOR_RGB(0x2A, 0x4A, 0x7E);
    ui_window_set_theme(win, &th);

    // --- MenuBar + Toolbar (bar full-width) ---
    ui_widget_t* bar = ui_menubar_create(win);
    ui_widget_t* m = ui_menubar_add_menu(bar, "File");
    ui_menu_add_item(m, "Baru",  on_menu, "Menu: File > Baru");
    ui_menu_add_item(m, "Buka",  on_menu, "Menu: File > Buka");
    ui_menu_add_item(m, "Tutup", on_menu, "Menu: File > Tutup");
    m = ui_menubar_add_menu(bar, "Edit");
    ui_menu_add_item(m, "Salin",  on_clip_copy, 0);   // Phase 9: clipboard
    ui_menu_add_item(m, "Tempel", on_clip_paste, 0);
    ui_window_add_bar(win, bar);

    ui_widget_t* tool = ui_toolbar_create(win);
    ui_toolbar_add_button(tool, "Muat",   on_menu, "Tool: Muat");
    ui_toolbar_add_button(tool, "Simpan", on_menu, "Tool: Simpan");
    ui_toolbar_add_button(tool, "Cari",   on_menu, "Tool: Cari");
    ui_toolbar_add_button(tool, "Dialog", on_dialog_btn, 0);   // Phase 9
    ui_toolbar_add_button(tool, "Notif",  on_notif_btn, 0);
    ui_window_add_bar(win, tool);

    // Phase 9: shortcut app — Ctrl+N → dialog. Daftar sebelum ui_window_run.
    ui_window_add_shortcut(win, KEY_MOD_CTRL, 'n', on_shortcut_new, 0);
    ui_window_add_shortcut(win, KEY_MOD_CTRL, 'N', on_shortcut_new, 0);

    ui_widget_t* box = ui_vbox_create(win, 8);
    status = ui_label_create(win, "Siap");
    ui_layout_add(box, status);

    // --- Tab ---
    ui_widget_t* tab = ui_tab_create(win, 320, 300);

    // Kontrol: regresi Phase 7 + Phase 6
    ui_widget_t* kb = ui_vbox_create(win, 8);
    lbl = ui_label_create(win, "Klik: 0");
    ui_layout_add(kb, lbl);
    ui_widget_t* btn = ui_button_create(win, "+1");
    ui_button_set_click(btn, on_plus, 0);
    ui_layout_add(kb, btn);
    teks_lbl = ui_label_create(win, "Teks: ");
    ui_layout_add(kb, teks_lbl);
    tb = ui_textbox_create(win, 160);
    ui_textbox_set_enter(tb, on_enter, 0);
    ui_layout_add(kb, tb);
    ui_widget_t* cb = ui_checkbox_create(win, "centang");
    ui_layout_add(kb, cb);
    slider = ui_slider_create(win, 0, 100);
    ui_slider_set_change(slider, on_slider, 0);
    ui_layout_add(kb, slider);
    progress = ui_progressbar_create(win, 160);
    ui_layout_add(kb, progress);
    ui_widget_t* img = ui_image_create(win, "kyuzen.png", 64, 64);
    ui_layout_add(kb, img);
    ui_tab_add(tab, "Kontrol", kb);

    // Daftar: ListView (20 item → scroll)
    lv = ui_listview_create(win, 320, 274);
    {
        char lab[24];
        for (int i = 1; i <= 20; i++) {
            cat_num(lab, "Item ", i);
            ui_listview_add_item(lv, lab);
        }
    }
    ui_listview_set_change(lv, on_list, 0);
    ui_tab_add(tab, "Daftar", lv);

    // Tabel: Table 3 kolom × 13 baris (header + scroll)
    tabel = ui_table_create(win, 320, 274);
    ui_table_add_column(tabel, "Nama", 90);
    ui_table_add_column(tabel, "Status", 60);
    ui_table_add_column(tabel, "Nilai", 60);
    for (unsigned i = 0; i < sizeof(row_data) / sizeof(row_data[0]); i++) {
        const char* cells[3];
        cells[0] = row_data[i].nama;
        cells[1] = row_data[i].status_;
        cells[2] = row_data[i].nilai;
        ui_table_add_row(tabel, cells, 3);
    }
    ui_table_set_change(tabel, on_table, 0);
    ui_tab_add(tab, "Tabel", tabel);

    // Pohon: TreeView dengan cabang collapsed (expand/collapse + scroll)
    pohon = ui_treeview_create(win, 320, 274);
    for (unsigned i = 0; i < sizeof(tree_data) / sizeof(tree_data[0]); i++)
        ui_treeview_add_node(pohon, tree_data[i].label,
                             tree_data[i].depth, tree_data[i].expanded);
    ui_treeview_set_change(pohon, on_tree, 0);
    ui_tab_add(tab, "Pohon", pohon);

    // Gulir: ScrollView membungkus VBox 15 label
    {
        ui_widget_t* sv = ui_scrollview_create(win, 320, 274);
        ui_widget_t* sb = ui_vbox_create(win, 8);
        char lab[24];
        for (int i = 1; i <= 15; i++) {
            cat_num(lab, "Baris ", i);
            ui_layout_add(sb, ui_label_create(win, lab));
        }
        ui_scrollview_set_child(sv, sb);
        ui_tab_add(tab, "Gulir", sv);
    }

    // Setelan: Phase 9 showcase — ScrollView membungkus VBox (dogfood Phase 8)
    {
        ui_widget_t* sv = ui_scrollview_create(win, 320, 274);
        ui_widget_t* sb = ui_vbox_create(win, 8);
        ui_widget_t* tbtn = ui_button_create(win, "Tema Gelap");
        ui_button_set_click(tbtn, on_theme, (void*)&tema_gelap);
        ui_layout_add(sb, tbtn);
        tbtn = ui_button_create(win, "Tema Terang");
        ui_button_set_click(tbtn, on_theme, (void*)&tema_terang);
        ui_layout_add(sb, tbtn);
        tbtn = ui_button_create(win, "Tema Hijau");
        ui_button_set_click(tbtn, on_theme, (void*)&tema_hijau);
        ui_layout_add(sb, tbtn);
        tbtn = ui_button_create(win, "Simpan Setelan");
        ui_button_set_click(tbtn, on_settings_save, 0);
        ui_layout_add(sb, tbtn);
        tbtn = ui_button_create(win, "Muat Setelan");
        ui_button_set_click(tbtn, on_settings_load, 0);
        ui_layout_add(sb, tbtn);
        tbtn = ui_button_create(win, "Tampilkan Dialog");
        ui_button_set_click(tbtn, on_dialog_btn, 0);
        ui_layout_add(sb, tbtn);
        tbtn = ui_button_create(win, "Notifikasi");
        ui_button_set_click(tbtn, on_notif_btn, 0);
        ui_layout_add(sb, tbtn);
        tbtn = ui_button_create(win, "Salin TextBox");
        ui_button_set_click(tbtn, on_clip_copy, 0);
        ui_layout_add(sb, tbtn);
        tbtn = ui_button_create(win, "Tempel ke TextBox");
        ui_button_set_click(tbtn, on_clip_paste, 0);
        ui_layout_add(sb, tbtn);
        // Drag & Drop: chip draggable + drop zone
        ui_widget_t* chip = ui_label_create(win, "Seret saya");
        ui_widget_set_draggable(chip, "chip:Setelan");
        ui_layout_add(sb, chip);
        ui_widget_t* zona = ui_label_create(win, "Jatuhkan di sini");
        ui_widget_set_drop_target(zona, on_drop, 0);
        ui_layout_add(sb, zona);
        ui_layout_add(sb, ui_label_create(win, "Kursor: TextBox = I-beam, tombol = tangan"));
        ui_scrollview_set_child(sv, sb);
        ui_tab_add(tab, "Setelan", sv);
    }

    ui_layout_add(box, tab);
    ui_window_add(win, box);

    ui_window_run(win);          // blocking; keluar via X titlebar / ESC
    ui_window_destroy(win);
    sys_exit();
}
