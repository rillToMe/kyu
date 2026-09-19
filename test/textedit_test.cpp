// ============================================================
// textedit_test.cpp — uji host untuk widget TextEdit (apps/libui.cpp).
//
// apps/libui.cpp dikompilasi apa adanya, lalu SEMUA simbol luar yang dipakai
// toolkit (syscalls + libgui + png, hanya 20 buah) di-stub di bawah. Jadi logika
// editor yang berisiko (undo/redo, seleksi, clipboard, cari/ganti, aritmetika
// word wrap) bisa diverifikasi di host, tanpa boot QEMU:
//
//   make test-textedit
//
// Yang TIDAK dicakup di sini (butuh event nyata): penanganan tombol di on_key
// (Shift+panah, drag-seleksi) — itu diuji lewat QEMU/klik. Yang diuji di sini
// adalah semua operasi yang dipanggil aplikasi editor (notepad).
// ============================================================
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Sama seperti apps/libui.cpp: userlib.h/libgui.h TIDAK punya extern "C" guard,
// jadi dibungkus di sini supaya nama symbol cocok dengan yang dipakai toolkit.
extern "C" {
#include "userlib.h"
#include "libgui.h"
}
#include "libui.h"

// ------------------------------------------------------------
// Stub syscall + libgui (dipakai toolkit)
// ------------------------------------------------------------
static uint64_t g_ms = 1000;

extern "C" {
void* sys_alloc(uint32_t n) { return malloc(n ? n : 1); }
void  sys_free(void* p)     { free(p); }
uint64_t sys_uptime(void)   { return g_ms; }
void  sys_yield(void)       {}
int   sys_get_event(kyuzen_event_t* e) { (void)e; return 0; }
int   sys_kwm_set_cursor(int k) { (void)k; return 0; }
int   sys_open(const char* p, uint32_t f) { (void)p; (void)f; return -1; }
int   sys_read_fd(int fd, void* b, uint32_t n) { (void)fd; (void)b; (void)n; return -1; }
int   sys_write_fd(int fd, const void* b, uint32_t n) { (void)fd; (void)b; (void)n; return -1; }
int   sys_close(int fd) { (void)fd; return 0; }

gui_window_t* gui_create_window(uint32_t w, uint32_t h) {
    static gui_window_t win;
    static uint32_t canvas[64 * 64];
    memset(&win, 0, sizeof(win));
    win.win_id = 1;
    win.width = w; win.height = h;
    win.inner_w = w; win.inner_h = h;
    win.canvas = canvas;
    win.is_running = 1;
    return &win;
}
void gui_destroy(gui_window_t* w) { (void)w; }
void gui_flush(gui_window_t* w) { (void)w; }
void gui_damage_rect(gui_window_t* w, int x, int y, int cw, int ch) {
    (void)w; (void)x; (void)y; (void)cw; (void)ch;
}
void gui_draw_rect(gui_window_t* w, int x, int y, int cw, int ch, uint32_t c) {
    (void)w; (void)x; (void)y; (void)cw; (void)ch; (void)c;
}
void gui_draw_text(gui_window_t* w, const char* t, int x, int y, uint32_t c) {
    (void)w; (void)t; (void)x; (void)y; (void)c;
}
void gui_draw_char(gui_window_t* w, char ch, int x, int y, uint32_t c) {
    (void)w; (void)ch; (void)x; (void)y; (void)c;
}
int gui_set_window_title(gui_window_t* w, const char* t) { (void)w; (void)t; return 0; }
uint32_t* png_decode(const char* f, int* w, int* h) { (void)f; (void)w; (void)h; return 0; }
void png_free(uint32_t* b) { (void)b; }
}   // extern "C"

// ------------------------------------------------------------
static int PASS = 0, FAIL = 0;
static void check(int cond, const char* what) {
    if (cond) { PASS++; printf("PASS %s\n", what); }
    else      { FAIL++; printf("FAIL %s\n", what); }
}

static ui_widget_t* mkedit(int chars_wide) {
    // Lebar widget menentukan kolom yang muat: (w-8)/8 kolom.
    return ui_textedit_create(0, chars_wide * 8 + 8, 160);
}

int main(void) {
    // --- dasar: insert, panjang, kursor, baris ---
    ui_widget_t* e = mkedit(20);
    ui_textedit_enable_undo(e, 1);
    check(ui_textedit_length(e) == 0, "awal: buffer kosong");
    check(ui_textedit_line_count(e) == 1, "awal: satu baris kosong");

    ui_textedit_insert(e, "halo");
    check(strcmp(ui_textedit_text(e), "halo") == 0, "insert: teks masuk di kursor");
    check(ui_textedit_length(e) == 4, "insert: panjang ikut naik");

    ui_textedit_insert(e, " dunia");
    check(strcmp(ui_textedit_text(e), "halo dunia") == 0, "insert: lanjut di akhir");
    ui_textedit_insert(e, "\nbaris2");
    check(ui_textedit_line_count(e) == 2, "enter: jumlah baris bertambah");

    int line = 0, col = 0;
    ui_textedit_cursor(e, &line, &col);
    check(line == 2 && col == 7, "kursor: posisi Ln=2 Col=7 (1-based)");
    ui_textedit_set_cursor(e, 4);
    ui_textedit_cursor(e, &line, &col);
    check(line == 1 && col == 5, "set_cursor: 4 → Ln 1 Col 5");
    check(ui_textedit_line_start_idx(e, 2) == 11, "line_start_idx: baris 2 mulai di 11");

    // --- undo / redo (per operasi, bukan per karakter buffer) ---
    ui_textedit_undo(e);                                   // buang "\nbaris2"
    check(strcmp(ui_textedit_text(e), "halo dunia") == 0, "undo: kembali satu aksi");
    ui_textedit_undo(e);                                   // buang " dunia"
    check(strcmp(ui_textedit_text(e), "halo") == 0, "undo: dua langkah");
    check(ui_textedit_can_redo(e) == 1, "undo: redo tersedia setelah undo");
    ui_textedit_redo(e);
    check(strcmp(ui_textedit_text(e), "halo dunia") == 0, "redo: mengembalikan aksi");
    ui_textedit_redo(e);
    check(strcmp(ui_textedit_text(e), "halo dunia\nbaris2") == 0, "redo: dua langkah");
    ui_textedit_undo(e);
    ui_textedit_undo(e);
    ui_textedit_undo(e);
    check(ui_textedit_length(e) == 0 && ui_textedit_can_undo(e) == 0,
          "undo: berhenti di keadaan awal (tidak melewati batas)");

    // Edit baru setelah undo membuang cabang redo (perilaku editor biasa).
    ui_textedit_insert(e, "satu");
    ui_textedit_undo(e);
    ui_textedit_insert(e, "dua");
    check(strcmp(ui_textedit_text(e), "dua") == 0 && ui_textedit_can_redo(e) == 0,
          "undo: cabang redo lama dibuang setelah edit baru");

    // --- seleksi + clipboard ---
    ui_textedit_set_text(e, "abcdef");
    ui_textedit_select(e, 2, 5);
    check(ui_textedit_has_sel(e) == 1 && ui_textedit_sel_length(e) == 3,
          "seleksi: 2..5 = 3 karakter");
    check(ui_textedit_copy(e) == 1, "copy: berhasil");
    check(strcmp(ui_clipboard_get_text(), "cde") == 0, "copy: isi clipboard = 'cde'");
    check(ui_textedit_delete_sel(e) == 1 && strcmp(ui_textedit_text(e), "abf") == 0,
          "hapus: seleksi hilang dari dokumen");
    ui_textedit_set_cursor(e, 2);          // "abf", sisipkan di indeks 2
    check(ui_textedit_paste(e) == 1 && strcmp(ui_textedit_text(e), "abcdef") == 0,
          "paste: menyisipkan clipboard di kursor (abf + cde → abcdef)");

    ui_textedit_set_text(e, "satu dua tiga");
    ui_textedit_select(e, 0, 4);
    check(ui_textedit_cut(e) == 1 && strcmp(ui_textedit_text(e), " dua tiga") == 0,
          "cut: seleksi terhapus");
    check(strcmp(ui_clipboard_get_text(), "satu") == 0, "cut: isi masuk clipboard");
    check(ui_textedit_delete_sel(e) == 0, "hapus: tanpa seleksi → 0 (bukan error)");

    // --- find / replace_all ---
    ui_textedit_set_text(e, "Budi dan budi dan Budi");
    check(ui_textedit_find(e, "budi", 0, 0) == 9, "find: case-sensitive kanan");
    check(ui_textedit_find(e, "budi", 0, 1) == 0, "find: ignore-case kiri");
    check(ui_textedit_find(e, "budi", 1, 0) == 9, "find: mulai dari offset");
    check(ui_textedit_find(e, "zzz", 0, 0) == -1, "find: tidak ketemu → -1");
    int n = ui_textedit_replace_all(e, "budi", "ani");
    check(n == 1, "replace_all: case-sensitive ganti 1 (sesuai Notepad)");
    check(strcmp(ui_textedit_text(e), "Budi dan ani dan Budi") == 0,
          "replace_all: hasil benar");
    n = ui_textedit_replace_all(e, "Budi", "Ani");
    check(n == 2 && strcmp(ui_textedit_text(e), "Ani dan ani dan Ani") == 0,
          "replace_all: dua tempat sekaligus");

    // Panjang pengganti berbeda (lebih panjang / lebih pendek) tidak merusak.
    ui_textedit_set_text(e, "aa-aa");
    check(ui_textedit_replace_all(e, "aa", "XXXX") == 2 &&
          strcmp(ui_textedit_text(e), "XXXX-XXXX") == 0,
          "replace_all: pengganti lebih panjang");
    ui_textedit_set_text(e, "XXXX-XXXX");
    check(ui_textedit_replace_all(e, "XXXX", "z") == 2 &&
          strcmp(ui_textedit_text(e), "z-z") == 0,
          "replace_all: pengganti lebih pendek");

    // --- word wrap: aritmetika baris LAYAR ---
    ui_widget_t* w10 = mkedit(10);            // 10 kolom per baris layar
    ui_textedit_set_text(w10, "abcdefghijklmnopqrstuvwxyz");   // 26 karakter
    check(ui_textedit_scroll_rows(w10) == 1, "wrap off: 26 karakter tetap 1 baris layar");
    ui_textedit_set_wrap(w10, 1);
    check(ui_textedit_scroll_rows(w10) == 3, "wrap on: 26 karakter / 10 kolom = 3 baris");
    ui_textedit_set_wrap(w10, 0);
    check(ui_textedit_scroll_rows(w10) == 1, "wrap off lagi: kembali 1 baris");

    // Lipatan di batas kata: "aaa bbb ccc" pada 10 kolom → "aaa bbb " + "ccc".
    ui_textedit_set_text(w10, "aaa bbb ccc");
    ui_textedit_set_wrap(w10, 1);
    check(ui_textedit_scroll_rows(w10) == 2, "wrap on: patah di spasi (2 baris)");

    // Kata tunggal yang lebih panjang dari layar tetap terpotong (tanpa spasi).
    ui_textedit_set_text(w10, "aaaaaaaaaaaaaaa");
    check(ui_textedit_scroll_rows(w10) == 2, "wrap on: kata panjang dipotong 10+5");

    // Baris baru eksplisit dihitung apa adanya.
    ui_textedit_set_text(w10, "satu\n\n tiga");
    check(ui_textedit_line_count(w10) == 3, "baris dokumen: 3 (ada baris kosong)");
    check(ui_textedit_scroll_rows(w10) == 3, "wrap on: baris pendek tidak dilipat");

    // --- kontrak error (defensive) ---
    ui_widget_t* ro = mkedit(20);
    ui_textedit_set_text(ro, "readonly");
    ui_textedit_set_readonly(ro, 1);
    ui_textedit_select(ro, 0, 3);
    check(ui_textedit_cut(ro) == 0, "readonly: cut ditolak");
    check(ui_textedit_paste(ro) == 0, "readonly: paste ditolak");
    check(ui_textedit_insert(ro, "x") == 0, "readonly: insert ditolak");
    check(strcmp(ui_textedit_text(ro), "readonly") == 0, "readonly: isi tidak berubah");
    check(ui_textedit_copy(ro) == 1, "readonly: copy tetap boleh");

    // Undo mati (terminal) → tidak ada langkah undo yang tercatat.
    ui_widget_t* nu = mkedit(20);
    ui_textedit_insert(nu, "abc");
    check(ui_textedit_can_undo(nu) == 0, "undo mati: tidak merekam langkah");
    check(ui_textedit_undo(nu) == 0, "undo mati: undo mengembalikan 0");

    // Buffer dibatasi 8K: menulis jauh lebih banyak tidak melampaui kapasitas.
    ui_widget_t* big = mkedit(40);
    for (int i = 0; i < 300; i++) ui_textedit_insert(big, "0123456789012345678901234567890");
    check(ui_textedit_length(big) <= 8191, "batas: panjang tetap <= 8191 (buffer 8K)");

    printf("\n%d PASS, %d FAIL\n", PASS, FAIL);
    return FAIL == 0 ? 0 : 1;
}
