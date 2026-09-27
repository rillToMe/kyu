// apps/widget_demo.c — demo Toolkit libui dari sisi C ABI.
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
#include "color_utils.h"   // COLOR_HEX/color_hex (libs/gui/color)

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
// Phase D: grup radio panel "Baru" (milik demo; dihancurkan sebelum window).
static ui_radio_group_t* g_phase_d_group = 0;

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
    COLOR_HEX(0x121212), COLOR_HEX(0xE0E0E0),
    COLOR_HEX(0xE94560), COLOR_HEX(0x0F3460),
    COLOR_HEX(0xFFFFFF), COLOR_HEX(0x2A4A7E),
};
static const ui_theme_t tema_terang = {
    COLOR_HEX(0xF0F0F0), COLOR_HEX(0x222222),
    COLOR_HEX(0xD32F2F), COLOR_HEX(0xCFD8DC),
    COLOR_HEX(0x222222), COLOR_HEX(0x90A4AE),
};
static const ui_theme_t tema_hijau = {
    COLOR_HEX(0x0D1F14), COLOR_HEX(0xDFF2E0),
    COLOR_HEX(0x4CAF50), COLOR_HEX(0x1B4D2E),
    COLOR_HEX(0xE8F5E9), COLOR_HEX(0x2E7D46),
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
    th.bg           = color_hex(0x121212);
    th.fg           = color_hex(0xE0E0E0);
    th.accent       = color_hex(0xE94560);
    th.button_bg    = color_hex(0x0F3460);
    th.button_fg    = color_hex(0xFFFFFF);
    th.button_hover = color_hex(0x2A4A7E);
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

    // Baru (Phase D): Radio + ComboBox + Separator + Grid + Tooltip.
    // Grup radio milik demo (dihancurkan sebelum window, pola menu konteks).
    {
        ui_widget_t* db = ui_vbox_create(win, 8);
        ui_layout_add(db, ui_label_create(win, "Radio:"));
        ui_radio_group_t* rg = ui_radio_group_create();
        ui_widget_t* r1 = ui_radio_create(win, "Pagi");
        ui_widget_t* r2 = ui_radio_create(win, "Siang");
        ui_widget_t* r3 = ui_radio_create(win, "Malam");
        ui_radio_set_group(r1, rg);
        ui_radio_set_group(r2, rg);
        ui_radio_set_group(r3, rg);
        ui_radio_set_selected(r1, 1);
        ui_layout_add(db, r1);
        ui_layout_add(db, r2);
        ui_layout_add(db, r3);
        ui_widget_t* cbx = ui_combobox_create(win, 160);
        ui_combobox_add_item(cbx, "Merah");
        ui_combobox_add_item(cbx, "Hijau");
        ui_combobox_add_item(cbx, "Biru");
        ui_combobox_set_selected(cbx, 0);
        ui_layout_add(db, cbx);
        ui_widget_t* sep = ui_separator_create(win, UI_SEP_HORIZONTAL);
        ui_widget_set_size(sep, 300, 1);
        ui_layout_add(db, sep);
        ui_widget_t* gr = ui_grid_create(win, 2, 2, 8);
        ui_widget_set_size(gr, 200, 64);
        ui_grid_put(gr, ui_button_create(win, "G1"), 0, 0);
        ui_grid_put(gr, ui_button_create(win, "G2"), 0, 1);
        ui_grid_put(gr, ui_button_create(win, "G3"), 1, 0);
        ui_grid_put(gr, ui_button_create(win, "G4"), 1, 1);
        ui_layout_add(db, gr);
        ui_widget_t* tip = ui_button_create(win, "Hover saya");
        ui_widget_set_tooltip(tip, "Ini tooltip Phase D");
        ui_layout_add(db, tip);
        ui_tab_add(tab, "Baru", db);
        g_phase_d_group = rg;
    }

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
    ui_radio_group_destroy(g_phase_d_group);
    g_phase_d_group = 0;
    ui_window_destroy(win);
    sys_exit();
}
