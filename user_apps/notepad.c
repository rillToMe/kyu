// ============================================================
// notepad.c — Notepad (gaya Notepad Windows 11): window bersih.
//
// Tata letak (atas → bawah):
//   [MenuBar: File  Edit  View  Help]   ← SEMUA perintah ada di dropdown ini
//   [editor teks mengisi sisa window]
//   [bar cari/ganti]                    ← tersembunyi; muncul lewat Ctrl+F/H
//   [status bar: Ln/Col ... N karakter | Teks biasa | 100%]
//
// Tidak ada toolbar, kotak nama berkas, atau baris tombol yang selalu tampak:
// itu yang membuat versi lama terlihat penuh. Nama berkas muncul di TITLE BAR
// (seperti Windows) dan File > Buka... / Simpan Sebagai... memakai dialog
// prompt (toolkit belum punya file dialog).
//
// Perintah:
//   File : Baru, Buka..., Simpan, Simpan Sebagai..., Keluar
//   Edit : Undo, Redo, Cut, Copy, Paste, Hapus, Cari..., Ganti..., Lompat ke
//          Baris..., Waktu/Tanggal, Pilih Semua   (Undo/Redo redup saat kosong)
//   View : Word Wrap (bercentang), Statistik Dokumen...
//   Help : Cara Pakai, Tentang Notepad
//
// Shortcut: Ctrl+N/O/S, Ctrl+Z/Y, Ctrl+X/C/V, Ctrl+A, Ctrl+F/H/G, Ctrl+W, Ctrl+T
// ESC: tutup bar cari (kalau terbuka), kalau sudah tertutup = keluar.
//
// Build: notepad.o + userlib.o + libgui.o + libui.o + png.o
// ============================================================
#include "userlib.h"
#include "libui.h"
// Palet warna (libs/color): tema ditulis per komponen lewat
// COLOR_RGB_INIT (bentuk initializer untuk tabel `static const`).
#include "color_types.h"

#define WIN_W      720
#define WIN_H      560
#define MENUBAR_H  24
#define MARGIN     8
#define SPACING    6
#define STATUS_H   20
#define FINDROW_H  28
#define MAXFILE    8191     // ui_textedit_set_text menyimpan maksimal 8K-1

// Tinggi editor dihitung dari window supaya tata letak selalu penuh —
// tanpa baris sisa di dasar saat bar cari muncul/hilang.
#define EDIT_W       (WIN_W - 2 * MARGIN)
#define EDIT_H_FULL  (WIN_H - 2 * MARGIN - MENUBAR_H - STATUS_H - 2 * SPACING)
#define EDIT_H_FIND  (EDIT_H_FULL - FINDROW_H - SPACING)

// Indeks item menu (untuk set_enabled / set_checked dari aplikasi).
enum { EDIT_UNDO = 0, EDIT_REDO = 1 };
enum { VIEW_WRAP = 0 };

// Tema Modern Dark (charcoal, BUKAN hitam murni):
//   editor #1E1E1E → chrome #2D2D2D (menubar/status) → panel #252526 (modal)
//   → tombol #3C3C3C, hover #4A4A4A. Toolkit menurunkan lapisan itu dari 6
//   warna ABI ini (lihat Theme::derive di apps/libui.cpp).
static const ui_theme_t NOTEPAD_THEME = {
    COLOR_RGB_INIT(0x2D, 0x2D, 0x2D),   // bg          — dasar window (di balik margin)
    COLOR_RGB_INIT(0xD4, 0xD4, 0xD4),   // fg          — teks isi (off-white, bukan putih penuh)
    COLOR_RGB_INIT(0x00, 0x98, 0xBC),   // accent      — centang menu (biru aksen halus); caret+shortcut
                                       //             diturunkan: caret ~#00E5FF, shortcut ~amber
    COLOR_RGB_INIT(0x1E, 0x1E, 0x1E),   // button_bg   — area teks utama ("kertas" editor)
    COLOR_RGB_INIT(0xD4, 0xD4, 0xD4),   // button_fg   — teks menu/status/tombol
    COLOR_RGB_INIT(0x3E, 0x3E, 0x42),   // button_hover — hover item menu + blok seleksi
};

static ui_window_t* g_win;
static ui_widget_t* g_edit;       // editor teks
static ui_widget_t* g_find;       // TextBox pola cari
static ui_widget_t* g_repl;       // TextBox teks pengganti
static ui_widget_t* g_findrow;    // HBox bar cari/ganti (tersembunyi by default)
static ui_widget_t* g_status;     // StatusBar
static ui_widget_t* g_edit_menu;  // dropdown Edit (Undo/Redo enabled)
static ui_widget_t* g_view_menu;  // dropdown View (centang Word Wrap)
static int g_find_open = 0;
static int g_dirty = 0;           // ada perubahan belum disimpan
static int g_wrap = 0;
static int g_from_fileman = 0;
static char g_file[64] = "catatan.txt";

// Aksi yang tertunda karena dokumen belum disimpan (lihat ask_save()).
enum { ACT_NONE = 0, ACT_NEW, ACT_OPEN, ACT_EXIT };
static int g_pending = ACT_NONE;

// ------------------------------------------------------------
// Prototipe
// ------------------------------------------------------------
static void do_new(void);
static void open_prompt(void);
static void menu_open(void* ud);
static void menu_exit(void* ud);
static void ask_save(const char* why, int action);
static void find_next(void);
static void find_row_show(void);
static void find_row_hide(void);
static void refresh_menus(void);

// ------------------------------------------------------------
// Utilitas string kecil (freestanding, tanpa <string.h>)
// ------------------------------------------------------------
static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

// Jejak ke konsol (COM1). Untuk uji headless (QEMU -display none) log serial
// satu-satunya bukti bahwa perintah benar-benar jalan.
static void trace(const char* tag, const char* arg, int num) {
    print((char*)"[notepad] ");
    print((char*)tag);
    if (arg) { print((char*)" "); print((char*)arg); }
    if (num >= 0) { print((char*)" = "); print_num((uint32_t)num); }
    print((char*)"\n");
}

static void scopy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = '\0';
}

// Tambahkan bilangan desimal ke buffer teks (tanpa printf).
static int put_num(char* dst, int n, int v) {
    char tmp[12];
    int k = 0;
    if (v < 0) { dst[n++] = '-'; v = -v; }
    if (v == 0) tmp[k++] = '0';
    while (v > 0) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
    for (int i = k - 1; i >= 0; i--) dst[n++] = tmp[i];
    return n;
}

static int put_str(char* dst, int n, const char* s) {
    for (int i = 0; s[i]; i++) dst[n++] = s[i];
    return n;
}

// ------------------------------------------------------------
// Judul window + status bar + keadaan menu
// ------------------------------------------------------------
static void update_title(void) {
    char t[96];
    int n = 0;
    if (g_dirty) n = put_str(t, n, "* ");
    n = put_str(t, n, g_file);
    n = put_str(t, n, " - Notepad");
    t[n] = '\0';
    ui_window_set_title(g_win, t);
}

// Undo/Redo redup saat tidak ada historis; centang Word Wrap mengikuti state.
static void refresh_menus(void) {
    ui_menu_set_enabled(g_edit_menu, EDIT_UNDO, ui_textedit_can_undo(g_edit));
    ui_menu_set_enabled(g_edit_menu, EDIT_REDO, ui_textedit_can_redo(g_edit));
    ui_menu_set_checked(g_view_menu, VIEW_WRAP, g_wrap);
}

// Toolkit memanggil ini setiap isi teks berubah (ketik, paste, undo, ...).
static void on_text_changed(void* ud) {
    (void)ud;
    if (!g_dirty) { g_dirty = 1; update_title(); }
    refresh_menus();
}

static int tick(void* ud) {
    (void)ud;
    static int last_sec = -1, last_len = -1, last_dirty = -1, last_line = -1, last_col = -1;
    uint32_t t[6];   // [year, month, day, hour, min, sec]
    sys_get_time(t);
    int len = ui_textedit_length(g_edit);
    int line = 0, col = 0;
    ui_textedit_cursor(g_edit, &line, &col);

    if ((int)t[5] == last_sec && len == last_len && g_dirty == last_dirty &&
        line == last_line && col == last_col) return 0;
    last_sec = (int)t[5]; last_len = len; last_dirty = g_dirty;
    last_line = line; last_col = col;

    char left[48];
    int n = 0;
    if (g_dirty) n = put_str(left, n, "* ");
    n = put_str(left, n, "Ln ");
    n = put_num(left, n, line);
    n = put_str(left, n, ", Col ");
    n = put_num(left, n, col);
    left[n] = '\0';

    char right[96];
    n = 0;
    n = put_num(right, n, len);
    n = put_str(right, n, " karakter  |  Teks biasa  |  100%");
    if (g_wrap) n = put_str(right, n, "  |  Wrap");
    right[n] = '\0';

    ui_statusbar_set_text(g_status, left, right);
    return 1;
}

// ------------------------------------------------------------
// Berkas
// ------------------------------------------------------------
static void toast(const char* s) { ui_window_notify(g_win, s, 2500); }

static int load_file(const char* name) {
    if (!name || !name[0]) return 0;
    if (!sys_file_exists((char*)name)) { toast("Berkas tidak ada"); return 0; }
    uint32_t sz = sys_file_size((char*)name);
    if (sz > MAXFILE) { toast("Berkas terlalu besar (maks 8 KB)"); return 0; }
    char* buf = (char*)sys_alloc(sz + 1);
    if (!buf) { toast("Kehabisan memori"); return 0; }
    buf[0] = '\0';
    if (sz > 0) sys_read_file_to_buffer((char*)name, buf, sz + 1);
    buf[sz] = '\0';
    ui_textedit_set_text(g_edit, buf);       // sekaligus reset historis undo
    sys_free(buf);
    scopy(g_file, name, sizeof(g_file));
    g_dirty = 0;
    update_title();
    refresh_menus();
    trace("muat", g_file, (int)sz);
    return 1;
}

static int save_file(void) {
    const char* name = g_file;
    if (!name[0]) { toast("Nama berkas kosong"); return 0; }
    const char* txt = ui_textedit_text(g_edit);
    int len = slen(txt);
    if (sys_file_exists((char*)name)) fs_delete((char*)name);
    if (!sys_create_file((char*)name, (char*)txt, len)) { toast("Gagal menyimpan"); return 0; }
    g_dirty = 0;
    update_title();
    refresh_menus();
    toast("Tersimpan");
    trace("simpan", g_file, len);
    return 1;
}

// Aksi sebenarnya (dipanggil langsung, atau setelah peringatan "belum disimpan").
static void do_new(void) {
    ui_textedit_set_text(g_edit, "");
    g_dirty = 0;
    update_title();
    refresh_menus();
    trace("dokumen-baru", 0, -1);
}

static void do_exit(void) { ui_window_request_close(g_win); }

// Tampilkan dialog simpan/buang. index: 0 = Simpan, 1 = Buang, -1 = ESC (batal).
static void on_unsaved(void* ud, int index) {
    (void)ud;
    int action = g_pending;
    g_pending = ACT_NONE;
    if (index < 0) return;                  // batal
    if (index == 0 && !save_file()) return;  // gagal simpan → jangan lanjut
    if (action == ACT_NEW)  do_new();
    if (action == ACT_OPEN) open_prompt();
    if (action == ACT_EXIT) do_exit();
}

static void ask_save(const char* why, int action) {
    static const char* btns[2] = { "Simpan", "Buang" };
    g_pending = action;
    ui_dialog_show(g_win, "Belum disimpan", why, btns, 2, on_unsaved, 0);
}

// Callback prompt nama berkas (File > Buka... / Simpan Sebagai...).
// text = 0 → dibatalkan.
static void on_open_name(void* ud, const char* text) {
    (void)ud;
    if (!text || !text[0]) { trace("buka-batal", 0, -1); return; }
    load_file(text);
}

static void on_saveas_name(void* ud, const char* text) {
    (void)ud;
    if (!text || !text[0]) { trace("simpan-sebagai-batal", 0, -1); return; }
    scopy(g_file, text, sizeof(g_file));
    update_title();
    save_file();
}

static void menu_new(void* ud) {
    (void)ud;
    if (g_dirty) { ask_save("Simpan perubahan dulu?", ACT_NEW); return; }
    do_new();
}

// Prompt nama berkas — dipisah dari menu_open karena pemanggil setelah
// peringatan "belum disimpan" harus melewati cek g_dirty (kalimat "Buang"
// tidak mengosongkan g_dirty, jadi cek itu akan bertanya tanpa henti).
static void open_prompt(void) {
    ui_prompt_show(g_win, "Buka berkas", "Nama berkas di KyuzenFS:", g_file, on_open_name, 0);
}

static void menu_open(void* ud) {
    (void)ud;
    if (g_dirty) { ask_save("Simpan perubahan dulu?", ACT_OPEN); return; }
    open_prompt();
}

static void menu_save(void* ud) { (void)ud; save_file(); }

static void menu_save_as(void* ud) {
    (void)ud;
    ui_prompt_show(g_win, "Simpan sebagai", "Nama berkas di KyuzenFS:", g_file, on_saveas_name, 0);
}

static void menu_exit(void* ud) {
    (void)ud;
    if (g_dirty) { ask_save("Simpan perubahan dulu?", ACT_EXIT); return; }
    do_exit();
}

// ------------------------------------------------------------
// Edit
// ------------------------------------------------------------
static void menu_undo(void* ud) {
    (void)ud;
    int ok = ui_textedit_undo(g_edit);
    if (!ok) toast("Tidak ada yang bisa di-undo");
    trace("undo", ok ? "ok" : "kosong", -1);
}
static void menu_redo(void* ud) {
    (void)ud;
    int ok = ui_textedit_redo(g_edit);
    if (!ok) toast("Tidak ada yang bisa di-redo");
    trace("redo", ok ? "ok" : "kosong", -1);
}
static void menu_cut(void* ud) {
    (void)ud;
    if (!ui_textedit_cut(g_edit)) toast("Pilih teks dulu (drag / Shift+panah)");
}
static void menu_copy(void* ud) {
    (void)ud;
    if (!ui_textedit_copy(g_edit)) toast("Pilih teks dulu (drag / Shift+panah)");
}
static void menu_paste(void* ud) {
    (void)ud;
    if (!ui_textedit_paste(g_edit)) toast("Clipboard kosong");
}
static void menu_delete(void* ud) {
    (void)ud;
    if (!ui_textedit_delete_sel(g_edit)) toast("Pilih teks dulu");
}
static void menu_sel_all(void* ud) { (void)ud; ui_textedit_sel_all(g_edit); }

// Menu Edit → Waktu/Tanggal (Windows memakai F5; di sini Ctrl+T karena
// tombol fungsi datang tanpa ASCII sehingga tak bisa jadi shortcut).
static void menu_time(void* ud) {
    (void)ud;
    uint32_t t[6];   // [year, month, day, hour, min, sec]
    sys_get_time(t);
    char stamp[32];
    int n = 0;
    n = put_num(stamp, n, (int)t[2]); stamp[n++] = '/';
    n = put_num(stamp, n, (int)t[1]); stamp[n++] = '/';
    n = put_num(stamp, n, (int)t[0]); stamp[n++] = ' ';
    n = put_num(stamp, n, (int)t[3]); stamp[n++] = ':';
    n = put_num(stamp, n, (int)t[4]);
    stamp[n] = '\0';
    if (!ui_textedit_insert(g_edit, stamp)) toast("Tidak bisa menyisipkan");
}

static int cursor_index(void) {
    // Indeks dokumen dari posisi kursor (line/col 1-based dari toolkit).
    int line = 0, col = 0;
    ui_textedit_cursor(g_edit, &line, &col);
    return ui_textedit_line_start_idx(g_edit, line) + (col - 1);
}

static void find_next(void) {
    const char* needle = ui_textbox_text(g_find);
    if (!needle[0]) { toast("Isi pola cari dulu"); return; }
    int from = cursor_index();
    int hit = ui_textedit_find(g_edit, needle, from + 1, 0);
    if (hit < 0) hit = ui_textedit_find(g_edit, needle, 0, 0);   // melingkar
    if (hit < 0) { toast("Tidak ditemukan"); trace("cari", needle, -1); return; }
    ui_textedit_select(g_edit, hit, hit + slen(needle));
    trace("cari", needle, hit);
}

static void menu_find_next(void* ud) { (void)ud; find_next(); }

// Bar cari/ganti: tersembunyi sampai Ctrl+F / Ctrl+H (gaya Notepad), jadi
// window tetap bersih saat hanya mengetik.
static void find_row_show(void) {
    if (g_find_open) { ui_window_focus(g_win, g_find); return; }
    g_find_open = 1;
    ui_widget_set_size(g_edit, EDIT_W, EDIT_H_FIND);
    ui_widget_set_visible(g_findrow, 1);
    ui_window_focus(g_win, g_find);
    trace("bar-cari", "buka", EDIT_H_FIND);
}

static void find_row_hide(void) {
    if (!g_find_open) return;
    g_find_open = 0;
    ui_widget_set_visible(g_findrow, 0);
    ui_widget_set_size(g_edit, EDIT_W, EDIT_H_FULL);
    ui_window_focus(g_win, g_edit);
    trace("bar-cari", "tutup", EDIT_H_FULL);
}

// Tombol "Tutup" di bar cari.
static void menu_find_close(void* ud) { (void)ud; find_row_hide(); }

// Edit > Cari... : munculkan bar lalu cari dari kursor.
static void menu_find(void* ud) {
    (void)ud;
    find_row_show();
    find_next();
}

// Edit > Ganti... : munculkan bar dan fokuskan kotak pengganti.
static void menu_replace(void* ud) {
    (void)ud;
    find_row_show();
    ui_window_focus(g_win, g_repl);
}

static void menu_replace_one(void* ud) {
    (void)ud;
    const char* needle = ui_textbox_text(g_find);
    const char* with = ui_textbox_text(g_repl);
    if (!needle[0]) { toast("Isi pola cari dulu"); find_row_show(); return; }
    // Kalau seleksi sekarang persis pola cari → timpa, lalu cari berikutnya.
    if (ui_textedit_has_sel(g_edit) && ui_textedit_sel_length(g_edit) == slen(needle)) {
        ui_textedit_insert(g_edit, with);
        find_next();
        return;
    }
    find_next();
}

static void menu_replace_all(void* ud) {
    (void)ud;
    const char* needle = ui_textbox_text(g_find);
    const char* with = ui_textbox_text(g_repl);
    if (!needle[0]) { toast("Isi pola cari dulu"); find_row_show(); return; }
    int n = ui_textedit_replace_all(g_edit, needle, with);
    trace("ganti-semua", needle, n);
    if (n <= 0) { toast("Tidak ditemukan"); return; }
    char b[40];
    int k = put_str(b, 0, "Diganti: ");
    k = put_num(b, k, n);
    k = put_str(b, k, " tempat");
    b[k] = '\0';
    toast(b);
}

// Edit > Lompat ke Baris... (Ctrl+G): prompt nomor baris.
static void on_goto_line(void* ud, const char* text) {
    (void)ud;
    if (!text || !text[0]) return;
    int n = 0;
    for (int i = 0; text[i] >= '0' && text[i] <= '9'; i++) n = n * 10 + (text[i] - '0');
    int lines = ui_textedit_line_count(g_edit);
    if (n <= 0 || n > lines) { toast("Nomor baris di luar rentang"); return; }
    ui_textedit_set_cursor(g_edit, ui_textedit_line_start_idx(g_edit, n));
    trace("lompat", 0, n);
}

static void menu_goto(void* ud) {
    (void)ud;
    ui_prompt_show(g_win, "Lompat ke baris", "Nomor baris:", "", on_goto_line, 0);
}

static void menu_stats(void* ud) {
    (void)ud;
    const char* t = ui_textedit_text(g_edit);
    int chars = slen(t), words = 0, in_word = 0;
    for (int i = 0; i < chars; i++) {
        int sp = (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t');
        if (!sp && !in_word) { words++; in_word = 1; }
        else if (sp) in_word = 0;
    }
    char b[160];
    int k = 0;
    k = put_str(b, k, "Karakter : "); k = put_num(b, k, chars);  b[k++] = '\n';
    k = put_str(b, k, "Kata     : "); k = put_num(b, k, words);  b[k++] = '\n';
    k = put_str(b, k, "Baris    : "); k = put_num(b, k, ui_textedit_line_count(g_edit));
    b[k] = '\0';
    static const char* btns[1] = { "Tutup" };
    ui_dialog_show(g_win, "Statistik dokumen", b, btns, 1, 0, 0);
}

// ------------------------------------------------------------
// View & bantuan
// ------------------------------------------------------------
static void menu_wrap(void* ud) {
    (void)ud;
    g_wrap = !g_wrap;
    ui_textedit_set_wrap(g_edit, g_wrap);
    refresh_menus();
    toast(g_wrap ? "Word wrap: AKTIF" : "Word wrap: MATI");
    // baris-LAYAR ikut dijejakkan: itu bukti wrap benar-benar melipat baris
    trace("wrap", g_wrap ? "on" : "off", ui_textedit_scroll_rows(g_edit));
}

// Suffix aksen: baris yang DIAWALI prefix ini digambar Toolkit dengan warna
// acc_text (amber ~#DCDCAA, gaya hint VS Code) — kombinasi tombol menonjol
// dari deskripsi fungsinya.
#define ACCENT_LINE "#> "

static void menu_about(void* ud) {
    (void)ud;
    static const char* btns[1] = { "Tutup" };
    ui_dialog_show(g_win, "Tentang Notepad",
                   "Notepad KyuzenOS\n"
                   "Editor teks polos (batas 8 KB)\n"
                   ACCENT_LINE "Ctrl+N/O/S  baru / buka / simpan\n"
                   ACCENT_LINE "Ctrl+Z/Y    undo / redo\n"
                   ACCENT_LINE "Ctrl+X/C/V  cut / copy / paste\n"
                   ACCENT_LINE "Ctrl+A      pilih semua\n"
                   ACCENT_LINE "Ctrl+F/H    cari / ganti\n"
                   ACCENT_LINE "Ctrl+G      lompat ke baris\n"
                   ACCENT_LINE "Ctrl+W      word wrap\n"
                   ACCENT_LINE "Ctrl+T      waktu/tanggal\n"
                   "Drag mouse atau Shift+panah = seleksi\n"
                   "Semua perintah lain ada di menu.",
                   btns, 1, 0, 0);
}

static void menu_help(void* ud) {
    (void)ud;
    static const char* btns[1] = { "Tutup" };
    ui_dialog_show(g_win, "Cara pakai",
                   "1. Nama berkas tampil di judul window.\n"
                   "2. File > Buka... / Simpan Sebagai... meminta\n"
                   "   nama berkas; File > Simpan menulis kembali\n"
                   "   ke berkas yang sama.\n"
                   "3. Ketik isi dokumen di area besar.\n"
                   "4. Ctrl+F memunculkan bar cari di bawah\n"
                   "   editor, Ctrl+H sekaligus kotak ganti.\n"
                   "   Tombol Tutup / ESC menyembunyikannya.\n"
                   "5. View > Word Wrap melipat baris panjang.\n"
                   "6. Keluar: File > Keluar atau ESC. Kalau ada\n"
                   "   perubahan, akan ditanya dulu.",
                   btns, 1, 0, 0);
}

// ESC: tutup bar cari dulu; kalau sudah tertutup = keluar (dengan pertanyaan
// simpan bila dokumen kotor).
static void on_escape(void* ud) {
    (void)ud;
    if (g_find_open) { find_row_hide(); return; }
    menu_exit(0);
}

// ------------------------------------------------------------
// main
// ------------------------------------------------------------
void main(void) {
    char fname[64] = "catatan.txt";
    if (sys_file_exists("edit.tmp")) {
        uint32_t ts = sys_file_size("edit.tmp");
        if (ts > 0 && ts < 63) {
            sys_read_file_to_buffer("edit.tmp", fname, sizeof(fname));
            fname[ts] = '\0';
            g_from_fileman = 1;
        }
        fs_delete("edit.tmp");
    }

    g_win = ui_window_create(WIN_W, WIN_H);
    if (!g_win) { sys_exit(); }
    ui_window_set_theme(g_win, &NOTEPAD_THEME);
    ui_window_set_title(g_win, "Notepad");

    // --- MenuBar: satu-satunya tempat perintah (bersih, gaya Windows) ---
    ui_widget_t* mb = ui_menubar_create(g_win);

    ui_widget_t* fm = ui_menubar_add_menu(mb, "File");
    ui_menu_add_item_acc(fm, "Baru",              "Ctrl+N", menu_new, 0);
    ui_menu_add_item_acc(fm, "Buka...",           "Ctrl+O", menu_open, 0);
    ui_menu_add_item_acc(fm, "Simpan",            "Ctrl+S", menu_save, 0);
    ui_menu_add_item_acc(fm, "Simpan Sebagai...", 0,        menu_save_as, 0);
    ui_menu_add_sep(fm);
    ui_menu_add_item_acc(fm, "Keluar",            0,        menu_exit, 0);

    g_edit_menu = ui_menubar_add_menu(mb, "Edit");
    ui_menu_add_item_acc(g_edit_menu, "Undo",             "Ctrl+Z", menu_undo, 0);
    ui_menu_add_item_acc(g_edit_menu, "Redo",             "Ctrl+Y", menu_redo, 0);
    ui_menu_add_sep(g_edit_menu);
    ui_menu_add_item_acc(g_edit_menu, "Cut",              "Ctrl+X", menu_cut, 0);
    ui_menu_add_item_acc(g_edit_menu, "Copy",             "Ctrl+C", menu_copy, 0);
    ui_menu_add_item_acc(g_edit_menu, "Paste",            "Ctrl+V", menu_paste, 0);
    ui_menu_add_item_acc(g_edit_menu, "Hapus",            "Del",    menu_delete, 0);
    ui_menu_add_sep(g_edit_menu);
    ui_menu_add_item_acc(g_edit_menu, "Cari...",          "Ctrl+F", menu_find, 0);
    ui_menu_add_item_acc(g_edit_menu, "Cari Berikutnya",  0,        menu_find_next, 0);
    ui_menu_add_item_acc(g_edit_menu, "Ganti...",         "Ctrl+H", menu_replace, 0);
    ui_menu_add_item_acc(g_edit_menu, "Lompat ke Baris...", "Ctrl+G", menu_goto, 0);
    ui_menu_add_item_acc(g_edit_menu, "Waktu/Tanggal",    "Ctrl+T", menu_time, 0);
    ui_menu_add_sep(g_edit_menu);
    ui_menu_add_item_acc(g_edit_menu, "Pilih Semua",      "Ctrl+A", menu_sel_all, 0);

    g_view_menu = ui_menubar_add_menu(mb, "View");
    ui_menu_add_item_acc(g_view_menu, "Word Wrap",        "Ctrl+W", menu_wrap, 0);
    ui_menu_add_sep(g_view_menu);
    ui_menu_add_item_acc(g_view_menu, "Statistik Dokumen...", 0,    menu_stats, 0);

    ui_widget_t* hm = ui_menubar_add_menu(mb, "Help");
    ui_menu_add_item(hm, "Cara Pakai", menu_help, 0);
    ui_menu_add_item(hm, "Tentang Notepad", menu_about, 0);

    ui_window_add_bar(g_win, mb);

    // --- Badan: editor + bar cari (tersembunyi) + status bar ---
    ui_widget_t* box = ui_vbox_create(g_win, SPACING);

    g_edit = ui_textedit_create(g_win, EDIT_W, EDIT_H_FULL);
    ui_textedit_enable_undo(g_edit, 1);
    ui_textedit_set_change(g_edit, on_text_changed, 0);
    ui_layout_add(box, g_edit);

    g_findrow = ui_hbox_create(g_win, SPACING);
    g_find = ui_textbox_create(g_win, 190);
    ui_layout_add(g_findrow, g_find);
    g_repl = ui_textbox_create(g_win, 160);
    ui_layout_add(g_findrow, g_repl);
    ui_widget_t* b1 = ui_button_create(g_win, "Berikutnya");
    ui_button_set_click(b1, menu_find_next, 0);
    ui_layout_add(g_findrow, b1);
    ui_widget_t* b2 = ui_button_create(g_win, "Ganti");
    ui_button_set_click(b2, menu_replace_one, 0);
    ui_layout_add(g_findrow, b2);
    ui_widget_t* b3 = ui_button_create(g_win, "Semua");
    ui_button_set_click(b3, menu_replace_all, 0);
    ui_layout_add(g_findrow, b3);
    ui_widget_t* b4 = ui_button_create(g_win, "Tutup");
    ui_button_set_click(b4, menu_find_close, 0);
    ui_layout_add(g_findrow, b4);
    ui_layout_add(box, g_findrow);

    g_status = ui_statusbar_create(g_win);
    ui_widget_set_size(g_status, EDIT_W, STATUS_H);
    ui_layout_add(box, g_status);

    ui_window_add(g_win, box);
    ui_widget_set_visible(g_findrow, 0);   // bar cari hanya muncul saat dicari

    // --- ESC: ditangani aplikasi (tutup bar cari dulu, baru keluar) ---
    ui_window_set_escape(g_win, on_escape, 0);

    // --- Shortcut ---
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'n', menu_new, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'N', menu_new, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'o', menu_open, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'O', menu_open, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 's', menu_save, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'S', menu_save, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'z', menu_undo, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'Z', menu_undo, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'y', menu_redo, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'Y', menu_redo, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'x', menu_cut, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'X', menu_cut, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'c', menu_copy, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'C', menu_copy, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'v', menu_paste, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'V', menu_paste, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'a', menu_sel_all, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'A', menu_sel_all, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'f', menu_find, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'F', menu_find, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'h', menu_replace, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'H', menu_replace, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'g', menu_goto, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'G', menu_goto, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'w', menu_wrap, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'W', menu_wrap, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 't', menu_time, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_CTRL, 'T', menu_time, 0);

    ui_window_set_tick(g_win, tick, 0);

    // Berkas awal: dari Explorer (edit.tmp) atau default catatan.txt.
    scopy(g_file, fname, sizeof(g_file));
    if (sys_file_exists(g_file)) load_file(g_file);
    g_dirty = 0;
    update_title();
    refresh_menus();
    ui_window_focus(g_win, g_edit);   // langsung bisa mengetik

    trace("mulai", g_file, ui_textedit_length(g_edit));

    ui_window_run(g_win);   // blocking; keluar via X / ESC / File > Keluar

    trace("keluar", 0, -1);
    ui_window_destroy(g_win);
    if (g_from_fileman) {
        char p[32];
        build_app_path(p, sizeof(p), "fileman.elf");
        sys_exec(p);
    } else {
        sys_exit();
    }
}
