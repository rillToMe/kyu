// ============================================================
// libui_owner_test.cpp — uji host propagasi owner + damage set_visible.
//
// Kenapa ada: Settings (sidebar + ScrollView berisi halaman yang di-show/
// hide) meninggalkan ghost halaman lama: foto wallpaper tetap tampil di
// halaman Appearance/Fonts. Penyebabnya di libs: ScrollView::child dan
// Tab::panels BUKAN anggota children[] sehingga owner (Window*) tak pernah
// mengalir ke subtree isi; Widget::set_visible melewatkan damage_full()
// bila owner null (libs/gui/widget/src/core/widget.cpp) → area halaman
// lama tak pernah digambar ulang.
//
// Test ini mengunci tiga hal (pola test-libui-theme: toolkit di-link,
// syscall/libgui di-stub, render nyata ke canvas + sampling piksel):
//   1. Owner mencapai subtree ScrollView dan Tab setelah ui_window_add.
//   2. set_visible(false/true) menandai damage SELURUH window.
//   3. End-to-end render: halaman tombol yang disembunyikan tak meninggalkan
//      piksel (area lama kembali warna bg).
//
//   make test-libui-owner
// ============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "userlib.h"
#include "libgui.h"
}

// ------------------------------------------------------------
// Stub syscall + libgui (pola libui_theme_test.cpp).
// ------------------------------------------------------------
#define GW_W 240
#define GW_H 120
static uint32_t g_canvas[GW_W * GW_H];
static gui_window_t g_win;

extern "C" {
void* sys_alloc(uint32_t n) { return malloc(n ? n : 1); }
void  sys_free(void* p)     { free(p); }
uint64_t sys_uptime(void)   { return 1000; }
void  sys_yield(void)       {}
int   sys_get_event(kyuzen_event_t* e) { (void)e; return 0; }
int   sys_kwm_set_cursor(int k) { (void)k; return 0; }
int   sys_open(const char* p, uint32_t f) { (void)p; (void)f; return -1; }
int   sys_read_fd(int fd, void* b, uint32_t n) { (void)fd; (void)b; (void)n; return -1; }
int   sys_write_fd(int fd, const void* b, uint32_t n) { (void)fd; (void)b; (void)n; return -1; }
int   sys_close(int fd) { (void)fd; return 0; }

gui_window_t* gui_create_window(uint32_t w, uint32_t h) {
    memset(&g_win, 0, sizeof(g_win));
    memset(g_canvas, 0, sizeof(g_canvas));
    g_win.win_id = 1;
    g_win.width = w > GW_W ? GW_W : w;
    g_win.height = h > GW_H ? GW_H : h;
    g_win.inner_w = g_win.width;
    g_win.inner_h = g_win.height;
    g_win.canvas = g_canvas;
    g_win.is_running = 1;
    return &g_win;
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
int gui_set_window_title(gui_window_t* w, const char* t) { (void)w; (void)t; return 0; }
uint32_t* png_decode(const char* f, int* w, int* h) {
    (void)f;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
}
void png_free(uint32_t* b) { free(b); }
}   // extern "C"

#include "core/widget.hpp"
#include "layout/vbox.hpp"
#include "primitives/button.hpp"
#include "primitives/label.hpp"
#include "containers/scrollview.hpp"
#include "containers/tab.hpp"
#include "window/window.hpp"

// ------------------------------------------------------------
static int PASS = 0, FAIL = 0;
static void check(int cond, const char* what) {
    if (cond) { PASS++; printf("PASS %s\n", what); }
    else      { FAIL++; printf("FAIL %s\n", what); }
}

static uint32_t at(int x, int y) { return g_canvas[y * GW_W + x] & 0xFFFFFFu; }

int main(void) {
    ui_window_t* win = ui_window_create(GW_W, GW_H);
    ui::Window* w = reinterpret_cast<ui::Window*>(win);

    // Struktur ala Settings: root VBox -> ScrollView -> pages VBox ->
    // pageA/pageB, dibangun DULU (owner null), ui_window_add TERAKHIR.
    ui_widget_t* root = ui_vbox_create(win, 4);
    ui_widget_t* scroll = ui_scrollview_create(win, 220, 100);
    ui_widget_t* pages = ui_vbox_create(win, 4);
    ui_widget_t* pageA = ui_vbox_create(win, 4);
    ui_widget_t* pageB = ui_vbox_create(win, 4);
    ui_widget_t* btnA = ui_button_create(win, "AAA");
    ui_widget_set_size(btnA, 100, 40);
    ui_layout_add(pageA, btnA);
    ui_layout_add(pageA, ui_label_create(win, "page A"));
    ui_layout_add(pageB, ui_label_create(win, "page B"));
    ui_layout_add(pages, pageA);
    ui_layout_add(pages, pageB);
    ui_scrollview_set_child(scroll, pages);
    ui_layout_add(root, scroll);
    ui_window_add(win, root);

    ui::Widget* pPages = reinterpret_cast<ui::Widget*>(pages);
    ui::Widget* pA = reinterpret_cast<ui::Widget*>(pageA);
    ui::Widget* pB = reinterpret_cast<ui::Widget*>(pageB);
    ui::Widget* pBtnA = reinterpret_cast<ui::Widget*>(btnA);

    // --- 1. Owner mengalir menembus ScrollView -------------------------
    check(pPages->owner == w, "owner: pages VBox (isi ScrollView) punya owner");
    check(pA->owner == w, "owner: pageA punya owner");
    check(pB->owner == w, "owner: pageB punya owner");
    check(pBtnA->owner == w, "owner: widget daun di page punya owner");

    // --- 2. Tab: owner mengalir ke panel -------------------------------
    ui_widget_t* tab = ui_tab_create(win, 220, 100);
    ui_widget_t* panel = ui_vbox_create(win, 4);
    ui_tab_add(tab, "T1", panel);
    ui::Widget* pPanel = reinterpret_cast<ui::Widget*>(panel);
    check(pPanel->owner == 0, "owner: panel pra-add masih null (sanity)");
    ui_window_add(win, tab);
    check(pPanel->owner == w, "owner: panel Tab punya owner setelah add");

    // --- 3. set_visible menandai damage seluruh window -----------------
    // Sembunyikan pageB dulu (tanpa damage path — siapkan kondisi awal).
    ui_widget_set_visible(pageB, 0);
    w->damage_full();
    w->render();   // render awal: canvas penuh terisi, damage bersih
    check(!w->dirty_valid, "damage: bersih setelah render awal");

    ui_widget_set_visible(pageA, 0);   // sembunyikan A (owner != null)
    check(w->dirty_valid && w->dirty_x == 0 && w->dirty_y == 0 &&
          w->dirty_w == GW_W && w->dirty_h == GW_H,
          "damage: hide page -> damage_full seluruh window");
    ui_widget_set_visible(pageB, 1);   // tampilkan B
    check(w->dirty_valid && w->dirty_x == 0 && w->dirty_y == 0 &&
          w->dirty_w == GW_W && w->dirty_h == GW_H,
          "damage: show page -> damage_full seluruh window");

    // --- 4. End-to-end: tak ada ghost halaman lama ---------------------
    w->render();   // tampilkan B penuh
    ui_widget_set_visible(pageB, 0);
    ui_widget_set_visible(pageA, 1);
    w->render();   // tampilkan A penuh
    int cx = pBtnA->x + pBtnA->w / 2, cy = pBtnA->y + pBtnA->h / 2;
    uint32_t btn_px = at(cx, cy);
    check(btn_px != at(2, 2), "render: tombol page A tergambar (beda dari bg)");
    ui_widget_set_visible(pageA, 0);
    ui_widget_set_visible(pageB, 1);
    w->render();   // tampilkan B: area A harus kembali bg
    check(at(cx, cy) == at(2, 2), "render: tak ada ghost — area A kembali bg");

    printf("libui_owner: %d PASS, %d FAIL\n", PASS, FAIL);
    return FAIL ? 1 : 0;
}
