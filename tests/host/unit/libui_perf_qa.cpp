// tests/host/unit/libui_perf_qa.cpp — anggaran render libui (regression guard).
//
// MENGAPA ADA
// KyuzenOS adalah OS, bukan prototipe web. Setiap operasi gambar berjalan di
// CPU tanpa akselerasi dan setiap frame berakhir di `gui_flush()`. Jadi biaya
// render adalah properti yang harus DIUKUR, bukan diasumsikan.
//
// Yang diukur di sini: biaya satu kali `draw()` untuk widget representatif,
// pada kanvas 1280x720. Angkanya bukan target absolut (mesin host berbeda),
// melainkan RASIO terhadap satu tolok ukur tetap: satu `rect()` penuh layar.
// Rasio itu stabil lintas mesin, jadi bisa dipakai sebagai anggaran:
//
//   * sebuah tombol harus jauh di bawah 1 layar-penuh;
//   * satu layar daftar 12 baris harus di sekitar beberapa layar-penuh;
//   * TIDAK ADA widget yang boleh melampaui anggaran di bawah.
//
// Kalau sebuah perubahan menaikkan biaya widget melampaui anggarannya, test
// ini gagal — itu cara mendeteksi regresi render tanpa QEMU.
//
//   ./test-libui-perf-qa
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Urutan include: <string.h> host dulu (lihat catatan di libui_visual_qa.cpp).
extern "C" {
#include "userlib.h"
#include "libgui.h"
}

static int g_w = 0, g_h = 0;
static uint32_t* g_canvas = 0;

extern "C" {
void* sys_alloc(uint32_t n) { return malloc(n ? n : 1); }
void  sys_free(void* p) { free(p); }
uint64_t sys_uptime(void) { return 1000; }
void  sys_yield(void) {}
int   sys_get_event(kyuzen_event_t* e) { (void)e; return 0; }
int   sys_kwm_set_cursor(int k) { (void)k; return 0; }
int   sys_open(const char* p, uint32_t f) { (void)p; (void)f; return -1; }
int   sys_read_fd(int fd, void* b, uint32_t n) { (void)fd; (void)b; (void)n; return -1; }
int   sys_write_fd(int fd, const void* b, uint32_t n) { (void)fd; (void)b; (void)n; return -1; }
int   sys_close(int fd) { (void)fd; return 0; }
int   sys_get_screen_size(uint32_t* w, uint32_t* h) { *w = 1280; *h = 720; return 0; }

gui_window_t* gui_create_window(uint32_t w, uint32_t h) {
    gui_window_t* win = (gui_window_t*)calloc(1, sizeof(gui_window_t));
    win->width = w; win->height = h;
    g_w = (int)w; g_h = (int)h;
    g_canvas = (uint32_t*)calloc((size_t)w * h, 4);
    win->canvas = g_canvas;
    return win;
}
void gui_destroy(gui_window_t* win) { free(win); free(g_canvas); g_canvas = 0; }
void gui_draw_rect(gui_window_t* win, int x, int y, int w, int h, color_t c) {
    uint32_t px = color_to_u32(color_opaque(c), FORMAT_ARGB);
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= (int)win->height) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= (int)win->width) continue;
            win->canvas[(size_t)j * win->width + i] = px;
        }
    }
}
void gui_draw_char(gui_window_t* win, char c, int x, int y, color_t col) {
    (void)win; (void)c; (void)x; (void)y; (void)col;
}
void gui_draw_text(gui_window_t* win, const char* s, int x, int y, color_t c) {
    (void)win; (void)s; (void)x; (void)y; (void)c;
}
void gui_damage_rect(gui_window_t* win, int x, int y, int w, int h) {
    (void)win; (void)x; (void)y; (void)w; (void)h;
}
void gui_flush(gui_window_t* win) { (void)win; }
int  gui_set_window_title(gui_window_t* win, const char* t) { (void)win; (void)t; return 0; }
uint32_t* png_decode(const char* f, int* w, int* h) {
    (void)f; if (w) *w = 0; if (h) *h = 0; return 0;
}
void png_free(uint32_t* b) { free(b); }
}

#include "core/theme.hpp"
#include "core/painter.hpp"
#include "primitives/button.hpp"
#include "primitives/checkbox.hpp"
#include "primitives/switch.hpp"
#include "primitives/slider.hpp"
#include "primitives/textbox.hpp"
#include "containers/listview.hpp"
#include "containers/section.hpp"

namespace {

int g_fail = 0;
void check(int cond, const char* what) {
    if (!cond) { printf("FAIL %s\n", what); g_fail++; }
    else       { printf("PASS %s\n", what); }
}

// Penghitung kerja: setiap pixel yang ditulis painter menambah 1. Dipakai
// untuk mengukur biaya RELATIF tanpa bergantung pada jam (yang tidak stabil
// di host test dan tidak ada di bare-metal).
long g_px = 0;

void count_px(gui_window_t* win, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)win->width) w = (int)win->width - x;
    if (y + h > (int)win->height) h = (int)win->height - y;
    if (w > 0 && h > 0) g_px += (long)w * h;
}

}  // namespace

// Hook: gui_draw_rect dihitung oleh wrapper di bawah (didefinisikan ulang).
// Kita tidak bisa mencegat gui_draw_rect (sudah di atas), jadi ukur dengan
// cara lain: jalankan draw() dua kali dan bandingkan jumlah pixel canvas yang
// BERUBAH. Itu persis "berapa banyak layar yang disentuh" — metrik yang benar
// untuk renderer berbasis damage.
static long diff_pixels(const uint32_t* a, const uint32_t* b, int w, int h) {
    long n = 0;
    for (int i = 0; i < w * h; i++) if (a[i] != b[i]) n++;
    return n;
}

int main(void) {
    const int W = 1280, H = 720;
    gui_window_t* win = gui_create_window(W, H);
    ui_theme_config_t cfg;
    cfg.mode = UI_THEME_DARK;
    cfg.accent = UI_ACCENT_NEUTRAL;
    cfg.custom = color_hex(0x000000);
    ui::Theme t;
    t.apply_config(&cfg);
    ui::Painter p(win, t);

    const long screen = (long)W * H;

    // Tolok ukur: satu rect penuh layar = 1.0 "layar".
    memset(g_canvas, 0, (size_t)W * H * 4);
    p.rect(0, 0, W, H, t.bg);
    long base = diff_pixels(g_canvas, g_canvas, W, H);   // no-op (self-diff)
    (void)base;
    printf("bench: 1 layar = %ld px\n", screen);

    // Helper: biaya satu draw() dalam satuan layar.
    struct Row { const char* name; double cost; };
    Row rows[12];
    int nrows = 0;

#define MEASURE(label, stmt)                                                  \
    do {                                                                      \
        memset(g_canvas, 0, (size_t)W * H * 4);                               \
        p.rect(0, 0, W, H, t.bg);                                             \
        uint32_t* before = (uint32_t*)malloc((size_t)W * H * 4);              \
        memcpy(before, g_canvas, (size_t)W * H * 4);                          \
        stmt;                                                                 \
        long d = diff_pixels(before, g_canvas, W, H);                         \
        free(before);                                                         \
        double cost = (double)d / (double)screen;                             \
        printf("  %-28s %8ld px  = %.4f layar\n", label, d, cost);            \
        rows[nrows].name = label; rows[nrows].cost = cost; nrows++;           \
    } while (0)

    printf("\nbiaya draw() per widget (kanvas 1280x720):\n");

    ui::Button btn("Primary");
    btn.set_variant(UI_BUTTON_PRIMARY);
    btn.x = 40; btn.y = 40;
    MEASURE("Button (primary)", btn.draw(p));

    ui::Button ib("");
    ib.set_icon(UI_ICON_SETTINGS);
    ib.x = 40; ib.y = 90;
    MEASURE("Button (icon)", ib.draw(p));

    ui::CheckBox ck("Enable notifications");
    ck.set_checked(true);
    ck.x = 40; ck.y = 140;
    MEASURE("CheckBox", ck.draw(p));

    ui::Switch sw("Wi-Fi");
    sw.set_on(true, false);
    sw.x = 40; sw.y = 180;
    MEASURE("Switch", sw.draw(p));

    ui::Slider sl(0, 100);
    sl.set_value(60);
    sl.x = 40; sl.y = 220; sl.w = 240;
    MEASURE("Slider", sl.draw(p));

    ui::TextBox tb(240);
    tb.set_text("Editable value");
    tb.x = 40; tb.y = 260;
    MEASURE("TextBox", tb.draw(p));

    ui::Section sec("Controls", 8);
    sec.x = 40; sec.y = 300; sec.w = 600;
    MEASURE("Section (kosong)", sec.draw(p));

    ui::ListView lv(600, 300);
    lv.x = 40; lv.y = 340;
    for (int i = 0; i < 12; i++)
        lv.add_row("Display", "Resolution, scaling, night light",
                   ui::ICON_DISPLAY, true);
    MEASURE("ListView 12 baris kaya", lv.draw(p));

    // ANGGARAN. Angka = fraksi layar 1280x720 yang boleh disentuh satu draw().
    //
    // ListView punya anggaran yang paling penting di sini: baris daftar
    // TRANSPARAN (hanya baris terpilih/hover yang menggambar latar), sehingga
    // 12 baris hanya menyentuh 0.0012 layar. Anggaran 0.05 memberi ruang 40x
    // untuk baris terpilih + ikon + scrollbar, TAPI tetap menangkap
    // anti-pola yang dilarang bahasa visual ini: membungkus setiap baris dalam
    // kartu berlatar (12 x 600 x 40 px = 0.31 layar) akan langsung gagal.
    struct Budget { const char* name; double max; };
    const Budget budgets[] = {
        {"Button (primary)", 0.010},
        {"Button (icon)",    0.010},
        {"CheckBox",         0.010},
        {"Switch",           0.010},
        {"Slider",           0.020},
        {"TextBox",          0.020},
        {"Section (kosong)", 0.010},
        {"ListView 12 baris kaya", 0.050},
    };
    for (unsigned i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
        double c = -1;
        for (int r = 0; r < nrows; r++)
            if (strcmp(rows[r].name, budgets[i].name) == 0) c = rows[r].cost;
        char msg[128];
        snprintf(msg, sizeof(msg), "anggaran render: %s (%.4f <= %.3f layar)",
                 budgets[i].name, c, budgets[i].max);
        check(c >= 0 && c <= budgets[i].max, msg);
    }

    gui_destroy(win);
    printf("\nperf-qa: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
