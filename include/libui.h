#ifndef LIBUI_H
#define LIBUI_H

#include <stdint.h>
#include "color_types.h"   // warna tema/API = color_t (libs/color)

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// libui — Widget Toolkit (Phase 6) — Public C ABI
//
// Aplikasi (C / Rust / Zig / dst.) berinteraksi dengan toolkit
// lewat API C murni ini. Semua handle OPAQUE — implementasi
// internal (libs/widget/, dulu apps/libui.cpp) adalah Modern C++ yang TIDAK bocor
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

// Tema — 6 warna `color_t` (libs/color). Byte alpha diabaikan painter (semua
// permukaan window opaque), jadi isi saja dengan COLOR_RGB(). Untuk tabel
// `static const` di C pakai bentuk initializer COLOR_RGB_INIT()/COLOR_WHITE_INIT.
//
// JALUR LEGACY (tetap didukung penuh): 6 warna eksplisit dari aplikasi.
// Toolkit memetakannya ke role semantik internal (surface = button_bg,
// surface_elevated = button_hover, selection = button_bg, focus = accent,
// ...); tampilan aplikasi legacy tidak berubah. Untuk tema mode+aksen baru
// pakai ui_theme_config_t + ui_window_set_theme_config di bawah.
typedef struct ui_theme {
    color_t bg;             // latar window
    color_t fg;             // teks umum
    color_t accent;         // aksen
    color_t button_bg;      // latar tombol
    color_t button_fg;      // teks tombol
    color_t button_hover;   // latar tombol saat hover
} ui_theme_t;

// --- Phase A: mode + aksen (non-breaking; ui_theme_t di atas tidak berubah) ---
// Kombinasi mode × aksen menurunkan palet semantik penuh secara deterministik
// (lihat docs/design/gui/ui-theme-system.md): Dark+Purple dsb. berasal dari
// mode = DARK + accent = PURPLE, bukan tema hardcoded terpisah.
typedef enum {
    UI_THEME_DARK = 0,      // charcoal netral (default)
    UI_THEME_LIGHT = 1      // terang first-class
} ui_theme_mode_t;

typedef enum {
    UI_ACCENT_NEUTRAL = 0,  // abu netral (default — tanpa ketergantungan biru)
    UI_ACCENT_BLUE = 1,     // aksen biru (opsional, bukan default)
    UI_ACCENT_PURPLE = 2,
    UI_ACCENT_GREEN = 3,
    UI_ACCENT_ORANGE = 4,
    UI_ACCENT_RED = 5,
    UI_ACCENT_CUSTOM = 6    // pakai field `custom` di bawah
} ui_theme_accent_t;

typedef struct ui_theme_config {
    ui_theme_mode_t mode;
    ui_theme_accent_t accent;
    color_t custom;         // base aksen bila accent == UI_ACCENT_CUSTOM
} ui_theme_config_t;

// Callback klik tombol. userdata = argumen ui_button_set_click.
typedef void (*ui_click_cb)(void* userdata);

// Callback klik yang MEMBUTUHKAN koordinat (window-local konten). Dipakai hook
// klik-kanan: pemanggil memakainya untuk tahu baris/sel mana yang diklik
// (mis. ui_table_row_at + penyaringan "area kosong").
typedef void (*ui_pos_click_cb)(void* userdata, int x, int y);

// Hook tombol mentah untuk APLIKASI (bukan widget). Dipanggil saat ada
// EVENT_KEY_PRESS dan TIDAK ada widget yang memegang fokus keyboard.
//   ascii    = P1 event (0 untuk tombol non-printable: F2, Delete, panah)
//   scancode = P3 event (bit 0x100 = tombol extended, mis. Delete/panah)
//   mods     = bitmask KEY_MOD_*
// Dipakai aplikasi tanpa widget input (File Manager: F2/Delete/panah) yang
// tidak bisa memakai ui_window_add_shortcut — registry shortcut hanya
// mencocokkan ASCII, sehingga tombol ber-P1 0 tidak akan pernah cocok.
// Setelah widget fokus ada (mis. kolom rename), hook ini berhenti dipanggil
// dan ketikan langsung ke widget itu — jadi tidak ada rebutan input.
typedef void (*ui_key_cb)(void* userdata, uint32_t ascii, uint32_t scancode, uint32_t mods);
void ui_window_set_key(ui_window_t* win, ui_key_cb cb, void* userdata);

// --- Window ---
// Buat window + pohon widget kosong. Return 0 jika gagal.
ui_window_t* ui_window_create(uint32_t width, uint32_t height);
void ui_window_destroy(ui_window_t* win);
void ui_window_set_theme(ui_window_t* win, const ui_theme_t* theme); // 0 = default
// Phase A: pasang tema dari mode + aksen (Dark/Light × 6 aksen + custom).
// Mengganti tema dari jalur legacy (dan sebaliknya) kapan saja; seluruh
// window digambar ulang. 0 = abaikan (tema tidak berubah).
void ui_window_set_theme_config(ui_window_t* win, const ui_theme_config_t* cfg);
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

// ESC global. Bila cb di-set, APLIKASI yang menentukan arti ESC (mis. Notepad:
// tutup bar cari dulu, baru keluar); bila tidak di-set, ESC menutup window.
void ui_window_set_escape(ui_window_t* win, ui_click_cb cb, void* userdata);

// --- Ikon (sistem ikon terpadu) ---
// Ikon digambar toolkit dari geometri vektor sederhana (bukan font ikon, bukan
// emoji, bukan aset). Satu gaya: goresan 1px pada grid 16px, sehingga ikon dari
// widget berbeda terasa satu keluarga. Aplikasi memakai NAMA SEMANTIK ini,
// bukan menggambar bentuknya sendiri.
//
// Nilai enum ini adalah kontrak ABI: JANGAN menyisipkan di tengah (tambahkan di
// akhir sebelum UI_ICON_COUNT) supaya aplikasi lama tetap benar.
enum {
    UI_ICON_NONE = 0,
    UI_ICON_CHEVRON_RIGHT = 1,
    UI_ICON_CHEVRON_DOWN = 2,
    UI_ICON_CHEVRON_LEFT = 3,
    UI_ICON_CHEVRON_UP = 4,
    UI_ICON_ARROW_RIGHT = 5,
    UI_ICON_EXPAND = 6,
    UI_ICON_COLLAPSE = 7,
    UI_ICON_CLOSE = 8,
    UI_ICON_CHECK = 9,
    UI_ICON_PLUS = 10,
    UI_ICON_MINUS = 11,
    UI_ICON_SEARCH = 12,
    UI_ICON_REFRESH = 13,
    UI_ICON_MORE = 14,
    UI_ICON_EDIT = 15,
    UI_ICON_TRASH = 16,
    UI_ICON_FOLDER = 17,
    UI_ICON_FILE = 18,
    UI_ICON_IMAGE = 19,
    UI_ICON_HOME = 20,
    UI_ICON_STAR = 21,
    UI_ICON_SETTINGS = 22,
    UI_ICON_DISPLAY = 23,
    UI_ICON_PALETTE = 24,
    UI_ICON_FONT = 25,
    UI_ICON_NETWORK = 26,
    UI_ICON_POWER = 27,
    UI_ICON_INFO = 28,
    UI_ICON_WARNING = 29,
    UI_ICON_ERROR = 30,
    // Tampilan daftar vs kisi (toggle view File Manager).
    UI_ICON_LIST = 31,
    UI_ICON_GRID = 32,
    UI_ICON_COUNT = 33
};

// --- Text provider: tipografi dari font sistem (aditif) ---
// Toolkit menggambar teks dengan bitmap 8×16 bawaan. Aplikasi yang punya akses
// ke font nyata (libs/text) dapat memasang PROVIDER proses, sehingga SEMUA teks
// toolkit — dan seluruh tata letak yang mengukur teks itu — memakai font
// tersebut. Inilah yang membuat pilihan font pengguna (Settings > Fonts)
// berlaku di seluruh aplikasi, bukan hanya di desktop.
//
// Kontrak:
//   * measure: lebar advance teks (n byte UTF-8) dalam px.
//   * line_height: jarak antar baris; ascent: jarak atas baris → baseline.
//   * draw: gambar pada (x, baseline_y) ke canvas ARGB8888 (stride = cw),
//     mengomposit `fg` DI ATAS `bg` (warna latar di belakang teks).
//     IDEMPOTEN WAJIB: menggambar teks yang sama dua kali pada posisi yang sama
//     harus menghasilkan pixel yang sama. Widget memang menggambar ulang area
//     yang sama (ScrollView menggambar anaknya dua kali per frame; hover
//     melukis ulang sebagian baris), dan provider yang mem-blend ke isi canvas
//     akan menggelapkan tepi glyph di setiap penggambaran ulang — teks tampak
//     menebal lalu "kembali normal" saat latar dilukis ulang.
//     Toolkit menandai damage-nya sendiri; provider TIDAK memanggil
//     gui_damage_rect().
// Provider tidak lengkap (measure/line_height/draw ada yang 0) diperlakukan
// sebagai "tidak ada" → toolkit kembali ke bitmap, bukan menggambar separuh.
// Pass NULL untuk melepas provider.
typedef int  (*ui_text_measure_fn)(void* ud, const char* text, int n);
typedef int  (*ui_text_metric_fn)(void* ud);
typedef void (*ui_text_draw_fn)(void* ud, uint32_t* canvas, int cw, int ch,
                                int x, int baseline_y, color_t fg, color_t bg,
                                const char* text);
typedef struct ui_text_provider {
    ui_text_measure_fn measure;
    ui_text_metric_fn  line_height;
    ui_text_metric_fn  ascent;       // boleh 0 (toolkit memakai default)
    ui_text_draw_fn    draw;
    void* ud;
} ui_text_provider_t;
void ui_text_provider_set(const ui_text_provider_t* provider);

// --- Label ---
// text di-copy oleh toolkit — caller boleh pakai stack buffer.
ui_widget_t* ui_label_create(ui_window_t* win, const char* text);
void ui_label_set_text(ui_widget_t* widget, const char* text);

// --- FtText: area teks FreeType (callback milik aplikasi) ---
// Toolkit TIDAK me-link FreeType: saat widget digambar, toolkit memanggil
// balik draw_cb dengan canvas mentah (XRGB8888, stride = cw) + origin
// widget (x, y, window-local konten). App meraster di dalamnya — biasanya
// satu panggilan kz_text_draw() (libs/text) — lalu damage diurus widget
// ini (refresh() -> mark_dirty -> Window::render -> gui_flush).
// Kontrak: app mengukur teks (kz_text_measure) dan memilih ukuran widget
// yang memuatnya; damage partial via refresh() setelah state berubah.
typedef void (*ui_fttext_draw_cb)(void* userdata, uint32_t* canvas,
                                  int cw, int ch, int x, int y);
ui_widget_t* ui_fttext_create(ui_window_t* win, int w, int h);
void ui_fttext_set_draw(ui_widget_t* widget, ui_fttext_draw_cb cb, void* userdata);
void ui_fttext_refresh(ui_widget_t* widget);   // tandai dirty -> gambar ulang
// Browser viewport: klik kiri + koordinat window-local (hit-test link).
void ui_fttext_set_click(ui_widget_t* widget, ui_pos_click_cb cb, void* userdata);

// --- Button ---
ui_widget_t* ui_button_create(ui_window_t* win, const char* text);
void ui_button_set_click(ui_widget_t* widget, ui_click_cb cb, void* userdata);
// Phase B: varian visual tombol (aditif, default SECONDARY bila tak dipanggil).
// PRIMARY = isi aksen (aksi utama dialog), SECONDARY = permukaan + border,
// DANGER = isi danger, TERTIARY = tanpa isi/border (aksi tenang: Batal,
// Lewati, tautan tindakan). Nilai di luar rentang = SECONDARY.
enum { UI_BUTTON_SECONDARY = 0, UI_BUTTON_PRIMARY = 1, UI_BUTTON_DANGER = 2,
       UI_BUTTON_TERTIARY = 3 };
void ui_button_set_variant(ui_widget_t* widget, int variant);
// Ikon pada tombol (aditif). icon = nilai UI_ICON_* (libui.h). 0 = hapus ikon.
// Tombol tanpa teks + berikon menjadi tombol ikon persegi (ukuran kontrol).
void ui_button_set_icon(ui_widget_t* widget, int icon);
// Phase B: enabled generik per-widget (aditif, default enabled). Disabled =
// digambar redup + tak menerima hover/klik/fokus; fokus yang sedang dipegang
// dilepas. Berlaku untuk Button/TextBox/CheckBox/Slider (widget lain
// mengabaikan secara visual tapi tetap tak bisa di-hit).
void ui_widget_set_enabled(ui_widget_t* widget, int enabled);
// Widget yang bisa DIKLIK tapi tidak boleh menjadi stop fokus keyboard.
// Dipakai mis. sidebar navigasi: barisnya menerima klik, tetapi panah
// atas/bawah dan F2/Delete tetap milik daftar isi + aplikasi. Berbeda dari
// disabled: widget tetap aktif penuh untuk mouse.
void ui_widget_set_focusable(ui_widget_t* widget, int focusable);

// --- Klik kanan (menu konteks) ---
// Widget menerima klik kanan (EVENT_MOUSE_CLICK P1=1) dan memanggil cb dengan
// koordinat window-local. Toolkit sendiri tidak menggambar menu: aplikasi
// memuat menu konteks lewat widget yang sudah ada (ui_menu_*).
void ui_widget_set_right_click(ui_widget_t* widget, ui_pos_click_cb cb, void* userdata);

// --- TextBox (Phase 7) ---
// Input teks satu baris, h=24. Klik memberi fokus (border accent);
// tombol masuk lewat EVENT_KEY_PRESS (P1 = char printable, P3 = scancode).
ui_widget_t* ui_textbox_create(ui_window_t* win, int width);
void ui_textbox_set_text(ui_widget_t* widget, const char* text);
const char* ui_textbox_text(ui_widget_t* widget);      // pointer buffer internal
void ui_textbox_set_enter(ui_widget_t* widget, ui_click_cb cb, void* userdata);
// Phase C: error state (state + rendering saja; validasi milik aplikasi).
// error != 0 → border danger (+ tint background); 0 → normal kembali.
// Coexist dengan fokus: border tetap focus ring saat fokus.
void ui_textbox_set_error(ui_widget_t* widget, int is_error);
// Tandai seluruh isi sebagai terpilih: tombol pengubah teks berikutnya
// MENGGANTI isi (bukan menambah) dan isinya digambar sebagai blok terpilih.
// Dipakai File Manager untuk ganti-nama inline (nama lama langsung bisa
// ditimpa, gaya Explorer). Isi kosong → tanpa efek.
void ui_textbox_select_all(ui_widget_t* widget);

// --- CheckBox (Phase 7) ---
// Kotak centang + label; klik men-toggle dan memanggil toggle_cb.
ui_widget_t* ui_checkbox_create(ui_window_t* win, const char* label);
void ui_checkbox_set_checked(ui_widget_t* widget, int checked);
int ui_checkbox_checked(ui_widget_t* widget);
void ui_checkbox_set_toggle(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- Radio (Phase D) ---
// Lingkaran + label; tepat satu terpilih per grup (klik/panah/Enter/Spasi).
// Tanpa grup = mandiri (klik memilih, tak bisa batal).
ui_widget_t* ui_radio_create(ui_window_t* win, const char* label);
void ui_radio_set_selected(ui_widget_t* widget, int selected);  // diam (tanpa change_cb)
void ui_radio_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);
// Grup: bukan widget (tanpa bounds/gambar/traversal); ownership eksplisit.
// Hancurkan grup → anggota terlepas; hancurkan radio → keluar grup.
typedef struct ui_radio_group ui_radio_group_t;
ui_radio_group_t* ui_radio_group_create(void);
void ui_radio_group_destroy(ui_radio_group_t* group);
void ui_radio_set_group(ui_widget_t* widget, ui_radio_group_t* group);  // 0 = lepas
ui_widget_t* ui_radio_get_selected(ui_radio_group_t* group);  // 0 = tak ada / grup 0

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

// --- ComboBox (Phase D) ---
// Kotak tertutup + popup daftar (reuse popup Menu). String disalin toolkit.
// change_cb dipanggil saat seleksi DIKOMIT (klik item / Enter / panah langsung);
// set_selected programatik diam. -1 = tak ada.
ui_widget_t* ui_combobox_create(ui_window_t* win, int width);
int ui_combobox_add_item(ui_widget_t* widget, const char* label);  // -> index / -1
int ui_combobox_remove_item(ui_widget_t* widget, int index);       // 1 = terhapus
void ui_combobox_clear(ui_widget_t* widget);
int ui_combobox_count(ui_widget_t* widget);
int ui_combobox_selected(ui_widget_t* widget);
void ui_combobox_set_selected(ui_widget_t* widget, int index);     // diam
void ui_combobox_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- Switch (toggle on/off) ---
// Beda dari CheckBox: CheckBox adalah bagian dari FORM (nilainya dikirim
// bersama tombol Simpan), Switch adalah pengaturan yang BERLAKU SEGERA.
// Bentuknya pun berbeda (track + knob) supaya perbedaannya terbaca.
ui_widget_t* ui_switch_create(ui_window_t* win, const char* label);
void ui_switch_set_on(ui_widget_t* widget, int on);   // programatik = tanpa callback
int  ui_switch_on(ui_widget_t* widget);
void ui_switch_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- Ikon: ukuran optik ---
// Ukuran standar ikon supaya layout yang disusun app sejajar dengan widget
// toolkit (ikon 16px = setinggi glyph teks).
enum { UI_ICON_SIZE_SM = 0, UI_ICON_SIZE_MD = 1, UI_ICON_SIZE_LG = 2,
       UI_ICON_SIZE_XL = 3 };
int ui_icon_size(int role);

// --- Section (bagian berjudul dalam halaman) ---
// Judul bagian memakai peran tipografi `section` (huruf besar, tone sekunder)
// diikuti garis tipis; isi disusun di bawahnya. Hierarki dibangun oleh
// tipografi, bukan oleh kartu/box — ini primitif utama halaman pengaturan.
// spacing = jarak antar anak (token UI_SPACE_*).
ui_widget_t* ui_section_create(ui_window_t* win, const char* title, int spacing);
void ui_section_set_title(ui_widget_t* widget, const char* title);

// --- Separator (Phase D) ---
// Garis visual non-interaktif (tebal 1px; panjang via ui_widget_set_size).
// Transparan terhadap mouse, tak focusable, tak masuk traversal.
enum { UI_SEP_HORIZONTAL = 0, UI_SEP_VERTICAL = 1 };
ui_widget_t* ui_separator_create(ui_window_t* win, int orientation);

// --- Tooltip (Phase D) ---
// Teks bantuan per-widget (disalin; 0/"" menghapus). Presentasional: tampil
// setelah hover 600ms, tanpa fokus/traversal, hilang saat pointer pindah.
void ui_widget_set_tooltip(ui_widget_t* widget, const char* text);

// --- Image (Phase 7) ---
// Menampilkan PNG dari KyuzenFS, diskalakan nearest-neighbor ke rect w×h.
// File hilang / decode gagal -> kotak kosong (bukan crash).
ui_widget_t* ui_image_create(ui_window_t* win, const char* filename, int w, int h);
// Phase 10: zoom — target display = natural PNG × percent/100 (10..400).
// Ukuran widget dihitung ulang; perlu di-scroll bila melebihi view.
void ui_image_set_scale(ui_widget_t* widget, int percent);
// Phase 10: ganti file PNG yang ditampilkan (viewer galeri), reset zoom 100%.
void ui_image_set_file(ui_widget_t* widget, const char* filename);
// Phase 11: skala otomatis agar SELURUH gambar masuk area view — dipakai viewer
// saat membuka gambar supaya tidak perlu digeser manual. Return persen yang
// dipakai (0 = tidak ada gambar), sudah di-clamp ke rentang ui_image_set_scale.
int  ui_image_set_fit(ui_widget_t* widget, int view_w, int view_h);
// Ukuran natural PNG yang sedang tampil (0,0 bila kosong/gagal decode).
void ui_image_natural_size(ui_widget_t* widget, int* out_w, int* out_h);

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
void ui_textedit_set_prompt_style(ui_widget_t* widget, const char* prefix, color_t color);

// --- TextEdit: API editor (Phase 11 — dipakai notepad) ---
// Semua mutasi lewat satu jalur (apply_replace) sehingga undo selalu konsisten.
// Aplikasi editor memakai shortcut (ui_window_add_shortcut) untuk Ctrl+A/C/X/V/Z/Y;
// widget sendiri yang menangani seleksi mouse-drag + Shift+panah/Home/End/PgUp/PgDn.
// Callback dipanggil setiap isi teks berubah (modified flag + status bar).
void ui_textedit_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);
// Aktifkan undo/redo historis operasi (0 = mati). Wajib dipanggil sebelum edit.
void ui_textedit_enable_undo(ui_widget_t* widget, int ops);
int  ui_textedit_undo(ui_widget_t* widget);       // 1 = teks berubah
int  ui_textedit_redo(ui_widget_t* widget);       // 1 = teks berubah
int  ui_textedit_can_undo(ui_widget_t* widget);
int  ui_textedit_can_redo(ui_widget_t* widget);
// Seleksi (blok teks): drag mouse / Shift+navigasi mengisinya sendiri.
void ui_textedit_sel_all(ui_widget_t* widget);
void ui_textedit_select(ui_widget_t* widget, int from, int to);
int  ui_textedit_has_sel(ui_widget_t* widget);
int  ui_textedit_sel_length(ui_widget_t* widget);
int  ui_textedit_delete_sel(ui_widget_t* widget);   // 1 = ada yang dihapus
// Clipboard (ui_clipboard_*): 1 = berhasil.
int  ui_textedit_copy(ui_widget_t* widget);
int  ui_textedit_cut(ui_widget_t* widget);
int  ui_textedit_paste(ui_widget_t* widget);
int  ui_textedit_insert(ui_widget_t* widget, const char* text);   // di kursor
// Info untuk status bar (line/col 1-based, gaya Notepad).
int  ui_textedit_length(ui_widget_t* widget);
int  ui_textedit_line_count(ui_widget_t* widget);
int  ui_textedit_line_start_idx(ui_widget_t* widget, int line1);
void ui_textedit_cursor(ui_widget_t* widget, int* line, int* col);
void ui_textedit_set_cursor(ui_widget_t* widget, int idx);
// Cari/ganti: index (-1 = tidak ketemu). ignore_case != 0 = abaikan besar-kecil.
int  ui_textedit_find(ui_widget_t* widget, const char* needle, int from, int ignore_case);
int  ui_textedit_replace_all(ui_widget_t* widget, const char* needle, const char* with);
// Word wrap (Format → Word Wrap): memperpendek baris panjang di layar.
void ui_textedit_set_wrap(ui_widget_t* widget, int on);
int  ui_textedit_wrap(ui_widget_t* widget);
int  ui_textedit_scroll_rows(ui_widget_t* widget);   // jumlah baris LAYAR

// --- Layout ---
// Tampil/sembunyi widget tanpa menghapusnya. VBox/HBox MELEWATI anak yang
// tersembunyi (tidak makan ruang), jadi baris opsional (bar cari Notepad)
// bisa muncul-hilang tanpa menulis ulang tata letak.
void ui_widget_set_visible(ui_widget_t* widget, int visible);
// Posisi window-local widget (x/y kiri-atas). Browser: origin FtText untuk
// hit-test link (widget digeser ScrollView saat scroll).
void ui_widget_pos(ui_widget_t* widget, int* out_x, int* out_y);
// Phase D: perataan dalam sel/kontainer (Grid; VBox/HBox tetap start).
enum { UI_ALIGN_START = 0, UI_ALIGN_CENTER = 1, UI_ALIGN_END = 2, UI_ALIGN_STRETCH = 3 };
// Phase D: mode ukuran track Grid (kolom/baris).
enum { UI_TRACK_AUTO = 0, UI_TRACK_FIXED = 1, UI_TRACK_FILL = 2 };
// Phase D: padding kontainer (kiri/atas/kanan/bawah, px). Nilai negatif
// dijepit 0 oleh toolkit.
typedef struct ui_padding {
    int left, top, right, bottom;
} ui_padding_t;
// Phase D: skala spacing hasil audit UI (4/8/12/16/24 — dipakai konsisten di
// toolkit + aplikasi; bukan sistem desain baru).
enum { UI_SPACE_XS = 4, UI_SPACE_SM = 8, UI_SPACE_MD = 12,
       UI_SPACE_LG = 16, UI_SPACE_XL = 24 };
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
// Bar full-width di DASAR window (StatusBar). Dipisah dari ui_window_add_bar
// karena bar puncak ditumpuk dari atas: statusbar yang dipasang lewat
// ui_window_add_bar akan muncul di bawah toolbar, bukan di dasar window.
void ui_window_add_bottom_bar(ui_window_t* win, ui_widget_t* bar);

// --- ScrollView ---
// Wadah scrollable generik: satu widget anak, scroll roda + scrollbar.
ui_widget_t* ui_scrollview_create(ui_window_t* win, int w, int h);
void ui_scrollview_set_child(ui_widget_t* widget, ui_widget_t* child);
// Phase 11: mode "lihat gambar": scrollbar HORIZONTAL ikut aktif (kalau isi
// lebih lebar dari view), anak ditaruh di tengah saat lebih kecil dari view,
// dan saat ukuran anak berubah (zoom) titik tengah view dipertahankan — jadi
// membesarkan gambar tidak melompat ke pojok kiri-atas. Scrollbar cuma muncul
// saat isi benar-benar melebihi view, sesuai perilaku image viewer biasa.
void ui_scrollview_set_pan(ui_widget_t* widget, int on);
// Browser viewport: baca/atur offset scroll vertikal (hit-test + keyboard).
int ui_scrollview_scroll(ui_widget_t* widget);
void ui_scrollview_set_scroll(ui_widget_t* widget, int pos);

// --- ListView ---
// Daftar item vertikal (row 20px); klik memilih & memanggil change_cb.
ui_widget_t* ui_listview_create(ui_window_t* win, int w, int h);
void ui_listview_add_item(ui_widget_t* widget, const char* label);
// Baris daftar KAYA (aditif): judul + deskripsi + ikon depan + chevron.
// Baris yang punya `description` menjadi dua baris tinggi. Teks disalin.
// Return index, atau -1 bila daftar penuh.
int  ui_listview_add_row(ui_widget_t* widget, const char* title,
                         const char* description, int icon, int chevron);
// Ikon/trailing/disabled per baris (aditif). icon = UI_ICON_*; 0 = kosong.
void ui_listview_set_row_icon(ui_widget_t* widget, int index, int icon);
void ui_listview_set_row_trailing(ui_widget_t* widget, int index, int icon);
void ui_listview_set_row_disabled(ui_widget_t* widget, int index, int disabled);
int ui_listview_selected(ui_widget_t* widget);   // index item terpilih, -1 = tak ada
// Pilih item dari kode (mis. viewer membuka berkas dari Explorer); baris
// bergulir ke dalam view bila di luar. Index di luar rentang → tanpa efek.
void ui_listview_set_selected(ui_widget_t* widget, int index);
void ui_listview_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);

// --- Table ---
// Header tetap 24px + baris 20px scrollable; klik memilih & change_cb.
ui_widget_t* ui_table_create(ui_window_t* win, int w, int h);
void ui_table_add_column(ui_widget_t* widget, const char* title, int width);
void ui_table_add_row(ui_widget_t* widget, const char* const* cells, int n);
void ui_table_clear(ui_widget_t* widget);   // hapus semua baris (refresh)
// Teks yang digambar di tengah area baris saat tabel KOSONG (mis. "This folder
// is empty") — pasangan ui_gridview_set_empty_text, bukan baris palsu.
void ui_table_set_empty_text(ui_widget_t* widget, const char* text);
int ui_table_selected(ui_widget_t* widget);
void ui_table_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);
// Baris yang berada di bawah `y` (window-local) atau -1 bila area kosong /
// header / di luar widget. Dipakai menu konteks File Manager untuk membedakan
// "klik kanan pada entri" dari "klik kanan pada latar".
int  ui_table_row_at(ui_widget_t* widget, int y);
// Pilih baris dari kode (mis. pilih ulang item yang sama setelah refresh).
// Index -1 = tidak ada yang terpilih. Baris digulirkan masuk view bila perlu.
// TIDAK memanggil change_cb (pemanggilnya yang tahu — menghindari rekursi).
void ui_table_set_selected(ui_widget_t* widget, int index);
// Ikon kecil opsional di kiri kolom pertama (menu/list view File Manager).
// Pointer piksel NON-OWNING (ARGB8888) — toolkit hanya membaca saat draw().
// row = index baris, px = 0 untuk menghapus ikon baris itu.
void ui_table_set_row_icon(ui_widget_t* widget, int row, const uint32_t* px, int w, int h);

// --- GridView ---
// Kisi item berlabel + thumbnail. Thumbnail MILIK APLIKASI (pointer non-owning):
// toolkit tidak mengenal filesystem/decoder/cache — pemanggil menyuplai piksel,
// toolkit menggambar kisi + menangani seleksi, scroll, hover, dan navigasi
// keyboard, serta memberi tahu sel mana yang TERLIHAT (dasar virtualisasi:
// only-visible-thumbnails, tanpa satu widget per item).
// Satu widget untuk seluruh kisi; ukuran sel tetap (kisi seragam).
// Placeholder sel sebelum thumbnail siap: UI_GRID_PH_*.
enum { UI_GRID_PH_EMPTY = 0, UI_GRID_PH_LOADING = 1, UI_GRID_PH_ERROR = 2 };
ui_widget_t* ui_gridview_create(ui_window_t* win, int w, int h);
// Ukuran pitch sel + kotak thumbnail di dalamnya (thumbnail diskalakan di sisi
// app, jadi toolkit hanya mem-blit 1:1).
void ui_gridview_set_cell(ui_widget_t* widget, int cell_w, int cell_h, int thumb_box);
// Teks yang digambar di tengah kisi saat kosong (mis. "No images in /").
void ui_gridview_set_empty_text(ui_widget_t* widget, const char* text);
int  ui_gridview_add_item(ui_widget_t* widget, const char* name);   // -> index / -1
void ui_gridview_clear(ui_widget_t* widget);                        // buang semua item
void ui_gridview_set_thumb(ui_widget_t* widget, int index, const uint32_t* px, int w, int h);
void ui_gridview_set_placeholder(ui_widget_t* widget, int index, int state);
int  ui_gridview_count(ui_widget_t* widget);
int  ui_gridview_selected(ui_widget_t* widget);                     // -1 = tak ada
// Daftar keyboard: panah/Home/End/PgUp/PgDn. Enter / klik-kedua = activate.
void ui_gridview_set_selected(ui_widget_t* widget, int index);      // tanpa change_cb
// Sel di bawah (x,y) window-local, atau -1. Pasangan ui_table_row_at — dipakai
// menu konteks supaya tahu sel mana yang diklik kanan.
int  ui_gridview_cell_at(ui_widget_t* widget, int x, int y);
void ui_gridview_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);
void ui_gridview_set_activate(ui_widget_t* widget, ui_click_cb cb, void* userdata);
void ui_gridview_ensure_visible(ui_widget_t* widget, int index);
// Rentang sel terlihat (inklusif). Return 0 bila kosong.
int  ui_gridview_visible_range(ui_widget_t* widget, int* first, int* last);

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
// Phase C: judul disabled (tak bisa klik/panah/fokus, tampil redup).
// Index di luar rentang → tanpa efek.
void ui_tab_set_enabled(ui_widget_t* widget, int index, int enabled);

// --- Grid (Phase D) ---
// Kontainer baris × kolom (subclass Layout: ownership/dirty/traversal sama).
// Track: AUTO (isi) / FIXED (px) / FILL (sisa dibagi rata). Anak ditaruh via
// ui_grid_put (row-major manual); overlap/invalid diabaikan deterministik.
// Ukuran Grid ditentukan caller (ui_widget_set_size) seperti kontainer lain.
ui_widget_t* ui_grid_create(ui_window_t* win, int rows, int cols, int gap);
int ui_grid_put(ui_widget_t* grid, ui_widget_t* child, int row, int col);
int ui_grid_put_span(ui_widget_t* grid, ui_widget_t* child, int row, int col,
                     int row_span, int col_span);   // span dijepit muat
void ui_grid_set_col(ui_widget_t* grid, int col, int mode, int px);
void ui_grid_set_row(ui_widget_t* grid, int row, int mode, int px);
void ui_grid_set_padding(ui_widget_t* grid, ui_padding_t pad);
void ui_grid_set_align(ui_widget_t* grid, int align);   // UI_ALIGN_* dalam sel

// --- Menu konteks (popup mandiri) ---
// Menu yang TIDAK dipasang ke MenuBar — dipakai File Manager untuk menu klik
// kanan. Isi itemnya sama seperti menu biasa (ui_menu_add_item / _acc / _sep /
// _set_enabled / _set_checked). Menu ini milik PEMANGGIL (tidak ada parent
// layout yang membebaskannya; umurnya sampai proses selesai) — aplikasi cukup
// membuatnya sekali dan memakainya berulang.
// Tampilkan di koordinat window-local; popup tertutup sendiri saat item diklik,
// klik di luar, ESC, atau ui_window_popup_menu lagi.
ui_widget_t* ui_menu_create(ui_window_t* win);
void ui_window_popup_menu(ui_window_t* win, ui_widget_t* menu, int x, int y);

// --- MenuBar + Menu ---
// MenuBar = bar full-width dengan judul berlebar mengikuti teksnya (gaya menu
// aplikasi Windows), bukan dibagi rata selebar window.
ui_widget_t* ui_menubar_create(ui_window_t* win);
ui_widget_t* ui_menubar_add_menu(ui_widget_t* bar, const char* title);  // return Menu
void ui_menu_add_item(ui_widget_t* menu, const char* label, ui_click_cb cb, void* userdata);
// Item + kolom accelerator rata kanan ("Simpan          Ctrl+S").
void ui_menu_add_item_acc(ui_widget_t* menu, const char* label, const char* acc,
                          ui_click_cb cb, void* userdata);
// Garis pemisah antar kelompok item.
void ui_menu_add_sep(ui_widget_t* menu);
// Tanda centang di gutter kiri (View > Word Wrap).
void ui_menu_set_checked(ui_widget_t* menu, int index, int checked);
// Item redup & tidak bereaksi klik (Undo/Redo saat tidak ada historis).
void ui_menu_set_enabled(ui_widget_t* menu, int index, int enabled);
// Set state item lewat ID (nama dari XML), bukan nomor baris. Ini menghapus
// keharusan aplikasi menyimpan enum index yang harus disinkronkan dengan
// urutan penambahan item — sumber bug nyata saat menu bertambah.
// Return 1 bila item dengan id itu ada, 0 bila tidak (id salah/menu tanpa id).
int ui_menu_set_checked_id(ui_widget_t* menu, const char* id, int checked);
int ui_menu_set_enabled_id(ui_widget_t* menu, const char* id, int enabled);

// --- StatusBar ---
// Pita status di dasar window: teks kiri ("Ln 1, Col 1") + teks kanan
// ("0 karakter  |  Teks biasa  |  100%"). Ukuran di-set lewat ui_widget_set_size.
ui_widget_t* ui_statusbar_create(ui_window_t* win);
void ui_statusbar_set_text(ui_widget_t* widget, const char* left, const char* right);

// --- Toolbar ---
// Bar tombol full-width di bawah MenuBar.
ui_widget_t* ui_toolbar_create(ui_window_t* win);
void ui_toolbar_add_button(ui_widget_t* bar, const char* label, ui_click_cb cb, void* userdata);
// Tombol toolbar ber-ikon (aditif). icon = UI_ICON_*; 0 = teks saja, dan
// label "" menghasilkan tombol ikon rapat.
void ui_toolbar_add_button_icon(ui_widget_t* bar, const char* label, int icon,
                                ui_click_cb cb, void* userdata);

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

// --- Prompt (dialog + satu kolom input teks) ---
// Pengganti dialog berkas: File > Buka / Simpan Sebagai.
// cb(userdata, text): text = isi kolom saat OK, 0 bila dibatalkan (ESC/Batal).
// Pointer text hanya valid selama callback (toolkit menyalinnya ke buffer
// internal sebelum memanggil, lalu buffer itu dilepas setelah callback).
typedef void (*ui_prompt_cb)(void* userdata, const char* text);
void ui_prompt_show(ui_window_t* win, const char* title, const char* text,
                    const char* initial, ui_prompt_cb cb, void* userdata);

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
// Format file (tiga versi, ukuran beda sehingga bisa dibedakan):
//   v2 — tag "KTH2" (4 byte) + mode(1) + aksen(1) + custom(r,g,b,a) = 10 byte
//          (ditulis bila tema terakhir dari ui_window_set_theme_config;
//          palet diturunkan saat load)
//   v1 — tag "KTH1" (4 byte) + 6 × color_t (r,g,b,a) = 28 byte
//   v0 — 6 × uint32 0x00RRGGBB tanpa tag = 24 byte (file lama, tetap dibaca)
// Load menolak file yang bukan theme valid (ukuran tak dikenal, tag salah,
// enum di luar rentang, atau semua nol) dan memakai jalur v0 untuk file lama.
// Return 1 sukses, 0 gagal/tak ada file.
int ui_settings_save(ui_window_t* win);
int ui_settings_load(ui_window_t* win);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LIBUI_H
