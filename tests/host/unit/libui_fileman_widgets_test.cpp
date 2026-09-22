// ============================================================
// libui_fileman_widgets_test.cpp — test host KONTRAK TOOLKIT yang dipakai
// apps/filemanager/ (File Manager fase ini).
//
// Semua yang diuji di sini gampang rusak tanpa terlihat di UI, dan semuanya
// diandalkan File Manager secara langsung:
//
//   1. Menu — set_checked()/set_enabled() memakai index yang MENGHITUNG baris
//      pemisah. File Manager memakai index tetap untuk View/Go (VM_*, GM_*),
//      jadi kalau separator berhenti dihitung, centang pindah ke item salah.
//   2. Table — row_at() (hit-test baris vs header vs area kosong) = dasar
//      menu klik kanan & klik-ganda; set_selected() dari kode tidak memanggil
//      change_cb; ikon baris digambar di kolom pertama dan TIDAK bocor ke baris
//      berikutnya setelah clear()/refresh.
//   3. GridView — cell_at() (menu klik kanan di icon view), seleksi dari kode
//      termasuk pembatalan (-1), dan set_thumb(px = 0) → placeholder LOADING
//      (File Manager menggantinya jadi EMPTY karena tidak ada yang dimuat).
//   4. Window — hook tombol aplikasi (ui_window_set_key) dipakai HANYA saat
//      tidak ada widget fokus (TextBox kolom path/rename menang), dan klik
//      kanan diteruskan ke widget di bawah kursor dengan koordinat window-local
//      sehingga aplikasi bisa tahu baris mana yang diklik.
//
//   make test-libui-fileman
// ============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Sama seperti runtime platform: userlib.h/libgui.h tanpa extern "C" guard.
extern "C" {
#include "userlib.h"
#include "libgui.h"
}

// ------------------------------------------------------------
// Canvas palsu + stub syscall/libgui (20 symbol yang sama dipakai toolkit).
// gui_draw_rect benar-benar mengisi canvas supaya hasil render bisa diperiksa
// piksel-per-piksel; teks no-op (glyph tidak relevan untuk uji geometri).
// ------------------------------------------------------------
#define GW_W 360
#define GW_H 240
static uint32_t g_canvas[GW_W * GW_H];
static gui_window_t g_gw;

// Antrean event: ukuran 8 cukup untuk urutan uji (run() berhenti saat kosong).
static kyuzen_event_t g_q[8];
static int g_qn = 0, g_qi = 0;
static void push_ev(uint32_t type, int p1, int p2, int p3) {
    if (g_qn >= 8) return;
    g_q[g_qn].type = type;
    g_q[g_qn].param1 = p1;
    g_q[g_qn].param2 = p2;
    g_q[g_qn].param3 = p3;
    g_q[g_qn].win_id = 1;
    g_qn++;
}

extern "C" {
void* sys_alloc(uint32_t n) { return malloc(n ? n : 1); }
void  sys_free(void* p)     { free(p); }
uint64_t sys_uptime(void)   { return 1000; }
void  sys_yield(void)       {}
int   sys_get_event(kyuzen_event_t* e) {
    if (g_qi >= g_qn) return 0;
    *e = g_q[g_qi++];
    return 1;
}
int   sys_kwm_set_cursor(int k) { (void)k; return 0; }
int   sys_open(const char* p, uint32_t f) { (void)p; (void)f; return -1; }
int   sys_read_fd(int fd, void* b, uint32_t n) { (void)fd; (void)b; (void)n; return -1; }
int   sys_write_fd(int fd, const void* b, uint32_t n) { (void)fd; (void)b; (void)n; return -1; }
int   sys_close(int fd) { (void)fd; return 0; }

gui_window_t* gui_create_window(uint32_t w, uint32_t h) {
    memset(&g_gw, 0, sizeof(g_gw));
    memset(g_canvas, 0, sizeof(g_canvas));
    g_gw.win_id = 1;
    g_gw.width = w > GW_W ? GW_W : w;
    g_gw.height = h > GW_H ? GW_H : h;
    g_gw.inner_w = g_gw.width;
    g_gw.inner_h = g_gw.height;
    g_gw.canvas = g_canvas;
    g_gw.is_running = 1;
    return &g_gw;
}
void gui_destroy(gui_window_t* w) { (void)w; }
void gui_flush(gui_window_t* w) { (void)w; }
void gui_damage_rect(gui_window_t* w, int x, int y, int cw, int ch) {
    (void)w; (void)x; (void)y; (void)cw; (void)ch;
}
void gui_draw_rect(gui_window_t* w, int x, int y, int cw, int ch, color_t c) {
    uint32_t solid = color_to_u32(color_opaque(c), FORMAT_ARGB);
    for (int iy = y; iy < y + ch; iy++) {
        if (iy < 0 || iy >= (int)w->height) continue;
        for (int ix = x; ix < x + cw; ix++) {
            if (ix < 0 || ix >= (int)w->width) continue;
            w->canvas[iy * (int)w->width + ix] = solid;
        }
    }
}
void gui_draw_text(gui_window_t* w, const char* t, int x, int y, color_t c) {
    (void)w; (void)t; (void)x; (void)y; (void)c;
}
void gui_draw_char(gui_window_t* w, char ch, int x, int y, color_t c) {
    (void)w; (void)ch; (void)x; (void)y; (void)c;
}
int  gui_set_window_title(gui_window_t* w, const char* t) { (void)w; (void)t; return 0; }
uint32_t* png_decode(const char* f, int* w, int* h) { (void)f; (void)w; (void)h; return 0; }
void png_free(uint32_t* b) { free(b); }
}   // extern "C"

// ------------------------------------------------------------
// Tipe internal toolkit yang diuji + hook aplikasi (dipanggil toolkit).
// ------------------------------------------------------------
#include "core/theme.hpp"            // ui::Theme
#include "core/widget.hpp"           // ui::Widget
#include "containers/table.hpp"      // ui::Table
#include "containers/gridview.hpp"   // ui::GridView
#include "chrome/menu.hpp"           // ui::Menu
#include "primitives/textbox.hpp"    // ui::TextBox
#include "window/window.hpp"         // ui::Window

static int PASS = 0, FAIL = 0;
static void check(int cond, const char* what) {
    if (cond) { PASS++; printf("PASS %s\n", what); }
    else      { FAIL++; printf("FAIL %s\n", what); }
}
static uint32_t at_px(int x, int y) { return g_canvas[y * (int)g_gw.width + x] & 0xFFFFFFu; }

// Callback change (dipakai untuk membuktikan set_selected() TIDAK memanggilnya).
static int g_change_n = 0;
static void on_change(void* ud) { (void)ud; g_change_n++; }

// Hook aplikasi: tombol + klik kanan.
static int g_key_n = 0;
static uint32_t g_key_ascii = 0, g_key_scan = 0, g_key_mods = 0;
static int g_rc_n = 0, g_rc_x = 0, g_rc_y = 0, g_rc_row = -99;
static ui_widget_t* g_tbl_for_rc = 0;

static void on_app_key(void* ud, uint32_t ascii, uint32_t scancode, uint32_t mods) {
    (void)ud;
    g_key_n++;
    g_key_ascii = ascii;
    g_key_scan = scancode;
    g_key_mods = mods;
}
static int g_enter_n = 0;
static void on_enter(void* ud) { (void)ud; g_enter_n++; }

static void on_widget_right(void* ud, int x, int y) {
    (void)ud;
    g_rc_n++;
    g_rc_x = x;
    g_rc_y = y;
    // Inilah yang dilakukan File Manager: dari koordinat window-local ke baris.
    g_rc_row = ui_table_row_at(g_tbl_for_rc, y);
}

int main(void) {
    ui_window_t* win = ui_window_create(GW_W, GW_H);
    if (!win) { printf("FAIL: window tidak bisa dibuat\n"); return 1; }
    ui::Window* w = reinterpret_cast<ui::Window*>(win);

    // ==========================================================
    // 1. Menu popup: index set_checked/set_enabled menghitung separator
    // ==========================================================
    {
        ui_widget_t* m = ui_menu_create(win);
        ui_menu_add_item(m, "List", 0, 0);        // 0
        ui_menu_add_item(m, "Icons", 0, 0);       // 1
        ui_menu_add_sep(m);                       // 2
        ui_menu_add_item(m, "Sidebar", 0, 0);     // 3
        ui::Menu* mm = reinterpret_cast<ui::Menu*>(m);
        ui_menu_set_checked(m, 3, 1);
        ui_menu_set_enabled(m, 0, 0);
        check(mm->n == 4 && mm->items[2].sep && mm->items[3].checked &&
              !mm->items[3].sep && mm->items[0].disabled && !mm->items[1].disabled,
              "menu: index set_checked/set_enabled menghitung baris pemisah "
              "(dasar index View/Go File Manager)");
    }

    // ==========================================================
    // 2. Table: row_at + seleksi dari kode + ikon baris
    // ==========================================================
    ui_widget_t* tbl = ui_table_create(win, 240, 120);
    ui_table_add_column(tbl, "Name", 150);
    ui_table_add_column(tbl, "Type", 90);
    const char* r0[2] = { "Pictures", "Folder" };
    const char* r1[2] = { "image.png", "Image" };
    const char* r2[2] = { "notes.txt", "Text" };
    const char* r3[2] = { "app.elf", "Application" };
    const char* r4[2] = { "data.bin", "File" };
    ui_table_add_row(tbl, r0, 2);
    ui_table_add_row(tbl, r1, 2);
    ui_table_add_row(tbl, r2, 2);
    ui_table_add_row(tbl, r3, 2);
    ui_table_add_row(tbl, r4, 2);
    ui::Table* tm = reinterpret_cast<ui::Table*>(tbl);
    tm->x = 10;
    tm->y = 20;
    tm->set_change(on_change, 0);
    g_tbl_for_rc = tbl;
    ui_widget_set_right_click(tbl, on_widget_right, 0);

    const int hdr = ui::Table::HEADER_H, rh = ui::Table::ROW_H;
    check(ui_table_row_at(tbl, tm->y - 1) == -1, "table: klik di luar widget bukan baris");
    check(ui_table_row_at(tbl, tm->y + hdr - 1) == -1, "table: klik di header bukan baris");
    check(ui_table_row_at(tbl, tm->y + hdr) == 0, "table: baris pertama tepat di bawah header");
    check(ui_table_row_at(tbl, tm->y + hdr + rh) == 1, "table: baris berikutnya = index 1");
    check(ui_table_row_at(tbl, tm->y + hdr + 5 * rh) == -1,
          "table: klik di bawah baris terakhir = area kosong (menu latar)");

    g_change_n = 0;
    ui_table_set_selected(tbl, 3);
    check(ui_table_selected(tbl) == 3 && g_change_n == 0,
          "table: set_selected() dari kode memilih baris tanpa memanggil change_cb");
    ui_table_set_selected(tbl, 99);
    check(ui_table_selected(tbl) == 3, "table: index di luar rentang diabaikan");
    ui_table_set_selected(tbl, -1);
    check(ui_table_selected(tbl) == -1, "table: -1 membatalkan seleksi (klik kanan di latar)");

    // Ikon baris: warna solid 16x16 → harus tampil di kolom pertama baris itu,
    // dan baris yang tak punya ikon harus tetap bersih (tidak bocor).
    uint32_t red[16 * 16];
    for (int i = 0; i < 16 * 16; i++) red[i] = 0xFFE53935u;
    uint32_t blue[16 * 16];
    for (int i = 0; i < 16 * 16; i++) blue[i] = 0xFF1E88E5u;
    ui_table_set_row_icon(tbl, 0, red, 16, 16);
    ui_table_set_row_icon(tbl, 1, blue, 16, 16);
    ui_table_set_row_icon(tbl, 2, 0, 0, 0);            // tanpa ikon

    // Table jadi anak root window: render() menata (x = 8, y = 8) + menggambar.
    ui_window_add(win, tbl);
    w->render();
    check(tm->x == 8 && tm->y == 8, "table: root VBox menaruh anak di (8,8)");
    check(at_px(tm->x + 2 + 8, tm->y + hdr + 2 + 8) == 0xE53935u,
          "table: ikon baris 0 digambar di kolom pertama (blit nyata)");
    check(at_px(tm->x + 2 + 8, tm->y + hdr + rh + 2 + 8) == 0x1E88E5u,
          "table: baris 1 memakai ikonnya sendiri");
    check(at_px(tm->x + 2 + 8, tm->y + hdr + 2 * rh + 2 + 8) != 0xE53935u &&
          at_px(tm->x + 2 + 8, tm->y + hdr + 2 * rh + 2 + 8) != 0x1E88E5u,
          "table: baris tanpa ikon tidak mewarisi ikon baris lain");

    ui_table_clear(tbl);
    check(tm->icon[0] == 0 && tm->icon[1] == 0 && tm->nrows == 0,
          "table: clear() membuang ikon baris (refresh tidak mewarisi ikon lama)");
    // Isi ulang seperti refresh File Manager (dir baru) untuk uji dispatch di §4.
    ui_table_add_row(tbl, r0, 2);
    ui_table_add_row(tbl, r1, 2);
    ui_widget_set_size(tbl, 240, 120);
    w->render();

    // ==========================================================
    // 3. GridView: cell_at + seleksi (+ deselect) + placeholder
    // ==========================================================
    {
        ui_widget_t* gv = ui_gridview_create(win, 240, 160);
        ui_gridview_set_cell(gv, 100, 90, 64);
        for (int i = 0; i < 7; i++) ui_gridview_add_item(gv, "item");
        ui::GridView* gm = reinterpret_cast<ui::GridView*>(gv);
        gm->x = 10;
        gm->y = 10;
        check(ui_gridview_count(gv) == 7, "gridview: 7 item masuk");

        int cx = 0, cy = 0;
        gm->cell_rect(2, cx, cy);
        check(ui_gridview_cell_at(gv, cx + 5, cy + 5) == 2,
              "gridview: cell_at mengembalikan sel di bawah kursor (klik kanan)");
        gm->cell_rect(0, cx, cy);
        check(ui_gridview_cell_at(gv, cx - 5, cy + 5) == -1,
              "gridview: klik di luar kisi bukan sel");
        check(ui_gridview_cell_at(gv, gm->x - 1, gm->y + 5) == -1,
              "gridview: klik di kiri widget bukan sel");

        ui_gridview_set_selected(gv, 5);
        check(ui_gridview_selected(gv) == 5, "gridview: set_selected dari kode");
        ui_gridview_set_selected(gv, 99);
        check(ui_gridview_selected(gv) == 5, "gridview: index di luar rentang diabaikan");
        ui_gridview_set_selected(gv, -1);
        check(ui_gridview_selected(gv) == -1,
              "gridview: -1 membatalkan seleksi (simetris dengan Table)");

        // Placeholder: set_thumb(px = 0) default LOADING; File Manager meminta
        // EMPTY karena tidak ada apa pun yang sedang dimuat untuk item itu.
        ui_gridview_set_thumb(gv, 0, 0, 0, 0);
        check(gm->cells[0].placeholder == ui::GridView::PH_LOADING,
              "gridview: thumbnail kosong = placeholder LOADING (bawaan)");
        ui_gridview_set_placeholder(gv, 0, UI_GRID_PH_EMPTY);
        check(gm->cells[0].placeholder == ui::GridView::PH_EMPTY && gm->cells[0].px == 0,
              "gridview: set_placeholder(EMPTY) = kotak kosong, bukan animasi memuat");

        uint32_t green[8 * 8];
        for (int i = 0; i < 8 * 8; i++) green[i] = 0xFF43A047u;
        ui_gridview_set_thumb(gv, 1, green, 8, 8);
        check(gm->cells[1].placeholder == ui::GridView::PH_EMPTY && gm->cells[1].px == green,
              "gridview: thumbnail terpasang menandai sel siap");

        ui_gridview_clear(gv);
        check(ui_gridview_count(gv) == 0 && ui_gridview_selected(gv) == -1,
              "gridview: clear() mengosongkan item + seleksi (refresh)");
    }

    // ==========================================================
    // 4. Window: hook tombol aplikasi + routing klik kanan
    // ==========================================================
    // Root sekarang memuat table (x=8, y=8 setelah arrange).
    ui_window_set_key(win, on_app_key, 0);

    // Baris 1 ada di y = 8 + HEADER_H + ROW_H .. +ROW_H.
    int rc_x = 8 + 12;
    int rc_y = 8 + hdr + rh + 6;
    push_ev(EVENT_MOUSE_MOVE, rc_x, rc_y, 0);
    push_ev(EVENT_MOUSE_CLICK, 1, 1, 0);          // klik kanan (P1=1) turun
    push_ev(EVENT_KEY_PRESS, 0, 0, 0x3C);         // F2: ascii 0, scancode 0x3C
    push_ev(EVENT_KEY_PRESS, 'r', KEY_MOD_CTRL, 0x13);
    push_ev(EVENT_WIN_CLOSE, 0, 0, 0);
    ui_window_run(win);                            // berhenti di EVENT_WIN_CLOSE

    check(g_rc_n == 1 && g_rc_x == rc_x && g_rc_y == rc_y,
          "window: klik kanan diteruskan ke widget di bawah kursor (koordinat window-local)");
    check(g_rc_row == 1,
          "window: koordinat klik kanan + ui_table_row_at = baris yang diklik (menu konteks)");
    check(g_key_n == 2, "window: hook tombol aplikasi menerima tombol saat tanpa widget fokus");
    check(g_key_ascii == 'r' && g_key_scan == 0x13 && (g_key_mods & KEY_MOD_CTRL) != 0,
          "window: hook menerima ascii/scancode/mods apa adanya (Ctrl+R)");

    // F2 saja (akhir urutan pertama) → ascii 0, scancode 0x3C (inilah alasan
    // File Manager tidak bisa memakai registry shortcut: P1 = 0).
    g_key_n = 0;
    g_qn = g_qi = 0;
    w->running = true;
    push_ev(EVENT_KEY_PRESS, 0, 0, 0x3C);
    push_ev(EVENT_WIN_CLOSE, 0, 0, 0);
    ui_window_run(win);
    check(g_key_n == 1 && g_key_ascii == 0 && g_key_scan == 0x3C,
          "window: F2 sampai ke aplikasi dengan scancode (P1 = 0, registry shortcut tak bisa)");

    // Widget fokus menang: kolom rename File Manager harus menerima ketikan,
    // dan hook aplikasi berhenti dipanggil (tidak ada rebutan input).
    {
        ui_widget_t* tb = ui_textbox_create(win, 140);
        ui_window_add(win, tb);
        ui_window_focus(win, tb);
        g_key_n = 0;
        g_qn = g_qi = 0;
        w->running = true;
        push_ev(EVENT_KEY_PRESS, 'x', 0, 0x2D);
        push_ev(EVENT_WIN_CLOSE, 0, 0, 0);
        ui_window_run(win);
        const char* txt = ui_textbox_text(tb);
        check(g_key_n == 0,
              "window: hook aplikasi TIDAK dipanggil saat ada widget fokus (bar rename)");
        check(txt && strchr(txt, 'x') != 0,
              "window: ketikan masuk ke widget fokus (kolom nama), bukan ke aplikasi");

        // Lepas fokus (release_focus() File Manager) → hook aplikasi hidup lagi.
        ui_window_focus(win, 0);
        g_key_n = 0;
        g_qn = g_qi = 0;
        w->running = true;
        push_ev(EVENT_KEY_PRESS, 0, 0, 0x153);    // Delete (extended)
        push_ev(EVENT_WIN_CLOSE, 0, 0, 0);
        ui_window_run(win);
        check(g_key_n == 1 && g_key_scan == 0x153,
              "window: setelah fokus dilepas, Delete (0x153) kembali ke aplikasi");
    }

    // ==========================================================
    // 5. TextBox: select_all() = ganti-nama inline (bar rename)
    // ==========================================================
    {
        ui::TextBox tbf(140);
        tbf.set_text("New Folder");
        tbf.select_all();
        check(tbf.replace_next, "textbox: select_all() menandai isi sebagai akan diganti");
        tbf.on_key('F', 0x21, 0);
        tbf.on_key('o', 0x18, 0);
        tbf.on_key('t', 0x14, 0);
        tbf.on_key('o', 0x18, 0);
        check(tbf.replace_next == false && strcmp(tbf.text, "Foto") == 0,
              "textbox: ketikan pertama MENGGANTI nama default (tanpa hapus manual)");

        tbf.on_key('X', 0x2D, 0);       // "FotoX"
        tbf.select_all();
        tbf.on_key(0, 0x0E, 0);         // Backspace pada isi terpilih
        check(tbf.replace_next == false && tbf.text[0] == '\0',
              "textbox: Backspace pada isi terpilih mengosongkan kolom");

        tbf.set_text("berkas.txt");
        tbf.select_all();
        tbf.enter_cb = on_enter;
        g_enter_n = 0;
        tbf.on_key(0, 0x1C, 0);         // Enter = commit nama apa adanya
        check(g_enter_n == 1 && strcmp(tbf.text, "berkas.txt") == 0,
              "textbox: Enter memakai isi apa adanya (commit ganti-nama)");
    }

    ui_window_destroy(win);
    printf("\n%d PASS, %d FAIL\n", PASS, FAIL);
    return FAIL == 0 ? 0 : 1;
}
