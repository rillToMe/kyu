#ifndef LIBUI_H
#define LIBUI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// libui — Widget Toolkit (Phase 6) — Public C ABI
//
// Aplikasi (C / Rust / Zig / dst.) berinteraksi dengan toolkit
// lewat API C murni ini. Semua handle OPAQUE — implementasi
// internal (apps/libui.cpp) adalah Modern C++ yang TIDAK bocor
// ke sini: tidak ada C++ type, template, exception, RTTI, atau
// STL di belakang API ini. Semua symbol luar tetap extern "C".
//
// Prefiks ui_ (bukan gui_) agar tidak bertabrakan dengan
// libgui.h yang sudah punya gui_window_t. libgui tetap renderer
// tingkat rendah (canvas + draw primitives); libui = pohon
// widget di atasnya.
//
// Alur:
//   ui_window_t* w = ui_window_create(280, 140);
//   ui_widget_t* box = ui_vbox_create(w, 10);
//   ui_widget_t* lbl = ui_label_create(w, "Klik: 0");
//   ui_layout_add(box, lbl);
//   ui_widget_t* btn = ui_button_create(w, "+1");
//   ui_button_set_click(btn, on_click, 0);
//   ui_layout_add(box, btn);
//   ui_window_add(w, box);
//   ui_window_run(w);          // blocking sampai window ditutup
//   ui_window_destroy(w);
// ============================================================

typedef struct ui_window ui_window_t;   // satu window + pohon widget
typedef struct ui_widget ui_widget_t;   // basis semua widget (label, button, layout)

// Tema — warna ARGB (alpha dipaksa 0xFF di painter).
typedef struct ui_theme {
    uint32_t bg;             // latar window
    uint32_t fg;             // teks umum
    uint32_t accent;         // aksen
    uint32_t button_bg;      // latar tombol
    uint32_t button_fg;      // teks tombol
    uint32_t button_hover;   // latar tombol saat hover
} ui_theme_t;

// Callback klik tombol. userdata = argumen ui_button_set_click.
typedef void (*ui_click_cb)(void* userdata);

// --- Window ---
// Buat window + pohon widget kosong. Return 0 jika gagal.
ui_window_t* ui_window_create(uint32_t width, uint32_t height);
void ui_window_destroy(ui_window_t* win);
void ui_window_set_theme(ui_window_t* win, const ui_theme_t* theme); // 0 = default
void ui_window_add(ui_window_t* win, ui_widget_t* widget);   // tambah ke layout root
void ui_window_run(ui_window_t* win);   // blocking sampai window ditutup (X / ESC)
// Minta event loop berhenti (mis. perintah `logout` di terminal). Efektif
// setelah iterasi loop saat ini; panggil dari dalam callback aman.
void ui_window_request_close(ui_window_t* win);

// Phase 10: judul window (titlebar WM + taskbar desktop).
void ui_window_set_title(ui_window_t* win, const char* title);

// Fokus keyboard intra-window ke widget (mis. TextBox terminal saat startup,
// agar ketikan langsung masuk tanpa perlu klik dulu).
void ui_window_focus(ui_window_t* win, ui_widget_t* widget);

// Phase 10: callback periodik tiap iterasi event loop (~60/s, via sys_yield
// + timer IRQ). Return 1 = ada perubahan → toolkit render; 0 = tetap.
// Dipakai jam / task manager untuk refresh tanpa event mouse/keyboard.
typedef int (*ui_tick_cb)(void* userdata);
void ui_window_set_tick(ui_window_t* win, ui_tick_cb cb, void* userdata);

// --- Label ---
// text di-copy oleh toolkit — caller boleh pakai stack buffer.
ui_widget_t* ui_label_create(ui_window_t* win, const char* text);
void ui_label_set_text(ui_widget_t* widget, const char* text);

// --- Button ---
ui_widget_t* ui_button_create(ui_window_t* win, const char* text);
void ui_button_set_click(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- TextBox (Phase 7) ---
// Input teks satu baris, h=24. Klik memberi fokus (border accent);
// tombol masuk lewat EVENT_KEY_PRESS (P1 = char printable, P3 = scancode).
ui_widget_t* ui_textbox_create(ui_window_t* win, int width);
void ui_textbox_set_text(ui_widget_t* widget, const char* text);
const char* ui_textbox_text(ui_widget_t* widget);      // pointer buffer internal
void ui_textbox_set_enter(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- CheckBox (Phase 7) ---
// Kotak centang + label; klik men-toggle dan memanggil toggle_cb.
ui_widget_t* ui_checkbox_create(ui_window_t* win, const char* label);
void ui_checkbox_set_checked(ui_widget_t* widget, int checked);
int ui_checkbox_checked(ui_widget_t* widget);
void ui_checkbox_set_toggle(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- Slider (Phase 7) ---
// Track horizontal + handle yang bisa diseret. w=160, h=20.
// change_cb dipanggil saat nilai berubah (klik langsung / drag).
ui_widget_t* ui_slider_create(ui_window_t* win, int min, int max);
void ui_slider_set_value(ui_widget_t* widget, int value);
int ui_slider_value(ui_widget_t* widget);
void ui_slider_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- ProgressBar (Phase 7) ---
// Fill horizontal 0..100, read-only. w ditentukan caller, h=16.
ui_widget_t* ui_progressbar_create(ui_window_t* win, int width);
void ui_progressbar_set_value(ui_widget_t* widget, int value);   // clamp 0..100

// --- Image (Phase 7) ---
// Menampilkan PNG dari KyuzenFS, diskalakan nearest-neighbor ke rect w×h.
// File hilang / decode gagal -> kotak kosong (bukan crash).
ui_widget_t* ui_image_create(ui_window_t* win, const char* filename, int w, int h);
// Phase 10: zoom — target display = natural PNG × percent/100 (10..400).
// Ukuran widget dihitung ulang; perlu di-scroll bila melebihi view.
void ui_image_set_scale(ui_widget_t* widget, int percent);
// Phase 10: ganti file PNG yang ditampilkan (viewer galeri), reset zoom 100%.
void ui_image_set_file(ui_widget_t* widget, const char* filename);

// --- TextEdit (Phase 10) ---
// Editor multi-baris. Buffer teks polos 8K; kursor + scroll roda/otomatis.
// Klik memberi fokus. readonly = tampilan output (terminal) — ketikan ditolak.
ui_widget_t* ui_textedit_create(ui_window_t* win, int w, int h);
void ui_textedit_set_text(ui_widget_t* widget, const char* text);
const char* ui_textedit_text(ui_widget_t* widget);      // pointer buffer internal
void ui_textedit_set_readonly(ui_widget_t* widget, int ro);
void ui_textedit_append(ui_widget_t* widget, const char* text);  // + auto-scroll bawah
void ui_textedit_clear(ui_widget_t* widget);
// Terminal shell: Enter memanggil cb (submit) alih-alih menyisip newline; kursor
// terkunci di baris perintah terakhir sehingga output lama tidak bisa diedit.
void ui_textedit_set_enter(ui_widget_t* widget, ui_click_cb cb, void* userdata);
// Terminal: baris yang DIAWALI `prefix` digambar dengan `prefix` berwarna
// `color` (gaya prompt shell Linux), sisanya tetap theme.fg. prefix dicopy
// oleh toolkit; 0/"" mematikan highlight. Dipakai terminal.c agar prompt
// "user@kyuzen:~$ " kontras terhadap output.
void ui_textedit_set_prompt_style(ui_widget_t* widget, const char* prefix, uint32_t color);

// --- Layout ---
// VBox: susun anaknya vertikal (masing-masing setinggi ukurannya,
// diberi spacing pixel). Win disediakan agar API seragam tapi
// widget hasilnya milik caller (bukan window).
ui_widget_t* ui_vbox_create(ui_window_t* win, int spacing);
// HBox (Phase 10): susun anaknya horizontal (grid tombol kalkulator).
ui_widget_t* ui_hbox_create(ui_window_t* win, int spacing);
// Paksa ukuran widget (tombol seragam dalam grid, display calc).
void ui_widget_set_size(ui_widget_t* widget, int w, int h);
void ui_layout_add(ui_widget_t* layout, ui_widget_t* child);

// --- Phase 8: Advanced Widgets ---
// Bar full-width di puncak window (MenuBar/Toolbar), di atas layout root.
void ui_window_add_bar(ui_window_t* win, ui_widget_t* bar);

// --- ScrollView ---
// Wadah scrollable generik: satu widget anak, scroll roda + scrollbar.
ui_widget_t* ui_scrollview_create(ui_window_t* win, int w, int h);
void ui_scrollview_set_child(ui_widget_t* widget, ui_widget_t* child);

// --- ListView ---
// Daftar item vertikal (row 20px); klik memilih & memanggil change_cb.
ui_widget_t* ui_listview_create(ui_window_t* win, int w, int h);
void ui_listview_add_item(ui_widget_t* widget, const char* label);
int ui_listview_selected(ui_widget_t* widget);   // index item terpilih, -1 = tak ada
void ui_listview_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- Table ---
// Header tetap 24px + baris 20px scrollable; klik memilih & change_cb.
ui_widget_t* ui_table_create(ui_window_t* win, int w, int h);
void ui_table_add_column(ui_widget_t* widget, const char* title, int width);
void ui_table_add_row(ui_widget_t* widget, const char* const* cells, int n);
void ui_table_clear(ui_widget_t* widget);   // hapus semua baris (refresh)
int ui_table_selected(ui_widget_t* widget);
void ui_table_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- TreeView ---
// Node ber-indent; marker '+'/'-' toggle expand/collapse; klik pilih node.
ui_widget_t* ui_treeview_create(ui_window_t* win, int w, int h);
void ui_treeview_add_node(ui_widget_t* widget, const char* label, int depth, int expanded);
int ui_treeview_selected(ui_widget_t* widget);
void ui_treeview_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- Tab ---
// Strip tab + panel aktif. Panel dimiliki oleh Tab (didelete saat destroy).
ui_widget_t* ui_tab_create(ui_window_t* win, int w, int h);
void ui_tab_add(ui_widget_t* widget, const char* title, ui_widget_t* panel);

// --- MenuBar + Menu ---
// MenuBar = bar full-width; ui_menubar_add_menu membuka dropdown (Menu).
ui_widget_t* ui_menubar_create(ui_window_t* win);
ui_widget_t* ui_menubar_add_menu(ui_widget_t* bar, const char* title);  // return Menu
void ui_menu_add_item(ui_widget_t* menu, const char* label, ui_click_cb cb, void* userdata);

// --- Toolbar ---
// Bar tombol full-width di bawah MenuBar.
ui_widget_t* ui_toolbar_create(ui_window_t* win);
void ui_toolbar_add_button(ui_widget_t* bar, const char* label, ui_click_cb cb, void* userdata);

// ============================================================
// Phase 9 — Desktop Services (Clipboard, Dialog, Notification,
// Drag & Drop, Cursor, Shortcut, Settings)
// ============================================================

// --- Clipboard ---
// Buffer teks global toolkit (satu app; cross-app butuh IPC kernel —
// sengaja di luar scope phase ini).
void ui_clipboard_set_text(const char* text);
const char* ui_clipboard_get_text(void);
void ui_clipboard_clear(void);

// --- Shortcut (accelerator per window) ---
// Cek di EVENT_KEY_PRESS sebelum dispatch ke widget fokus. mods = bitmask
// KEY_MOD_* (0 = tanpa modifier). Pencocokan: (mods event & 0x07) ==
// (mods & 0x07) && P1 == key. CapsLock diabaikan.
void ui_window_add_shortcut(ui_window_t* win, uint32_t mods, uint8_t key,
                            ui_click_cb cb, void* userdata);

// --- Dialog (async modal) ---
// Overlay modal di tengah window; blok input latar. cb(index) dipanggil saat
// tombol ditekan; index = -1 bila ditutup via ESC (batal). Non-blocking —
// cb async, konsisten dgn gaya callback toolkit (click_cb, change_cb, menu).
typedef void (*ui_dialog_cb)(void* userdata, int index);
void ui_dialog_show(ui_window_t* win, const char* title, const char* text,
                    const char* const* buttons, int n_buttons,
                    ui_dialog_cb cb, void* userdata);

// --- Notification (toast) ---
// Kotak kecil di pojok kanan-atas window; auto-expire setelah `ms` ms
// (sys_uptime). Klik pada toast menutupnya segera.
void ui_window_notify(ui_window_t* win, const char* text, uint32_t ms);

// --- Drag & Drop (intra-window) ---
// Widget yang di-set draggable memulai drag saat klik-tahan (bukan klik —
// click_cb-nya tidak dipanggil). Saat lepas di atas widget drop-target,
// drop_cb(payload, x, y) dipanggil; lepas di tempat lain = batal.
typedef void (*ui_drop_cb)(void* userdata, const char* payload, int x, int y);
void ui_widget_set_draggable(ui_widget_t* widget, const char* payload);
void ui_widget_set_drop_target(ui_widget_t* widget, ui_drop_cb cb, void* userdata);

// --- Cursor ---
// Bentuk kursor per-widget; Window mengganti kursor global kernel (syscall 58)
// saat widget di-hover berubah. TextBox = IBEAM, Button = HAND (bawaan).
enum { UI_CURSOR_ARROW = 0, UI_CURSOR_IBEAM = 1, UI_CURSOR_HAND = 2 };
void ui_widget_set_cursor(ui_widget_t* widget, int kind);

// --- Settings (persist theme ke KyuzenFS "settings.ui") ---
// Simpan/muat tema window sebagai blob 6×uint32. Load menolak file yang
// bukan theme valid (semua nol). Return 1 sukses, 0 gagal/tak ada file.
int ui_settings_save(ui_window_t* win);
int ui_settings_load(ui_window_t* win);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LIBUI_H
