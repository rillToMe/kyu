// ============================================================
// libui_theme_test.cpp — uji host TEMA + RENDER gradien tombol libui.
//
// Kenapa ada: saat warna libui dimigrasikan ke libs/color, gradien tombol sempat
// rata (bukan gradien) tanpa terlihat test lain. Penyebabnya warna tema dari ABI
// app (ui_theme_t) datang sebagai XRGB: byte alpha 0. color_blend_alpha membaca
// dst.a == 0 sebagai "kanvas kosong" (return src), jadi tiap baris gradien —
// yang di-blend di atas warna bawah bertema — mengembalikan warna src apa adanya.
//
// test ini mengunci dua hal:
//   1. Theme menormalkan 6 warna ABI jadi opaque (kontrak libui.h: "warna ARGB,
//      alpha dipaksa 0xFF di painter") + round-trip to_abi() tetap setia.
//   2. rrect_grad() tombol benar-benar bergradien dan tiap barisnya PERSIS sama
//      dengan yang digambar jalur lama aa_shade()/aa_mix() → migrasi warna
//      tidak mengubah satu piksel pun UI.
//
// Toolkit kini di libs/widget/ (dulu satu apps/libui.cpp yang di-INCLUDE di
// sini). Tipe internal yang diperiksa tetap di-include per-layer, lalu object
// toolkit-nya di-LINK oleh Makefile — pola yang sama dipakai test/kyuzenfs_v4_test.c.
//
//   make test-libui-theme
// ============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Sama seperti libs/widget/include/runtime/platform.hpp: userlib.h/libgui.h tanpa extern "C" guard.
extern "C" {
#include "userlib.h"
#include "libgui.h"
}

// ------------------------------------------------------------
// Stub syscall + libgui. gui_draw_rect di sini BENAR-BENAR mengisi canvas
// (seperti libgui asli) supaya hasil render bisa diperiksa piksel-per-piksel;
// teks di-stub no-op agar glyph tidak menutupi baris gradien.
// ------------------------------------------------------------
#define GW_W 240
#define GW_H 120
static uint32_t g_canvas[GW_W * GW_H];
static gui_window_t g_win;

// FS palsu: satu file di RAM (cukup untuk settings.ui) supaya format file
// bisa diuji: tulis, baca ulang, dan memuat file LAMA (v0 24 byte).
static unsigned char g_file[64];
static int g_file_len = 0;   // ukuran file
static int g_file_pos = 0;   // posisi baca/tulis
static int g_fd_open = 0;

// Isi file dengan pola u32 XRGB (format v0 lama).
static void fake_legacy_file(const uint32_t* v, int n) {
    g_file_len = n * 4;
    for (int i = 0; i < n * 4; i++) g_file[i] = (unsigned char)(v[i / 4] >> (8 * (i % 4)));
}

extern "C" {
void* sys_alloc(uint32_t n) { return malloc(n ? n : 1); }
void  sys_free(void* p)     { free(p); }
uint64_t sys_uptime(void)   { return 1000; }
void  sys_yield(void)       {}
int   sys_get_event(kyuzen_event_t* e) { (void)e; return 0; }
int   sys_kwm_set_cursor(int k) { (void)k; return 0; }
int   sys_open(const char* p, uint32_t f) {
    (void)p;
    if (f & O_CREAT) { g_file_len = 0; g_file_pos = 0; g_fd_open = 1; return 1; }
    if (g_file_len <= 0) return -1;          // tak ada file
    g_file_pos = 0; g_fd_open = 1;
    return 1;
}
int   sys_read_fd(int fd, void* b, uint32_t n) {
    (void)fd;
    if (!g_fd_open) return -1;
    int avail = g_file_len - g_file_pos;
    int take = (int)n < avail ? (int)n : avail;
    for (int i = 0; i < take; i++) ((unsigned char*)b)[i] = g_file[g_file_pos + i];
    g_file_pos += take;
    return take;
}
int   sys_write_fd(int fd, const void* b, uint32_t n) {
    (void)fd;
    if (!g_fd_open || g_file_pos + (int)n > (int)sizeof(g_file)) return -1;
    for (uint32_t i = 0; i < n; i++) g_file[g_file_pos + i] = ((const unsigned char*)b)[i];
    g_file_pos += (int)n;
    g_file_len = g_file_pos;
    return (int)n;
}
int   sys_close(int fd) { (void)fd; g_fd_open = 0; return 0; }

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
    // Seperti libgui asli: canvas selalu opaque (mask transparansi window
    // diurus compositor, bukan app).
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

// PNG palsu: dimensi di-set test supaya matematika fit/zoom viewer bisa diuji
// tanpa berkas gambar sungguhan (0 = decode gagal).
static int g_png_w = 0, g_png_h = 0;
uint32_t* png_decode(const char* f, int* w, int* h) {
    (void)f;
    if (w) *w = 0;
    if (h) *h = 0;
    if (g_png_w <= 0 || g_png_h <= 0) return 0;
    uint32_t* p = (uint32_t*)sys_alloc((uint32_t)(g_png_w * g_png_h * 4));
    if (!p) return 0;
    for (int i = 0; i < g_png_w * g_png_h; i++) p[i] = 0xFFFFFFFFu;
    if (w) *w = g_png_w;
    if (h) *h = g_png_h;
    return p;
}
void png_free(uint32_t* b) { free(b); }
}   // extern "C"

#include "aa_math.h"
// Tipe internal toolkit yang dipakai test ini (dulu semua lewat apps/libui.cpp).
#include "core/theme.hpp"             // ui::Theme
#include "core/painter.hpp"           // ui::Painter
#include "core/widget.hpp"            // ui::Widget
#include "primitives/button.hpp"      // ui::Button
#include "primitives/image.hpp"       // ui::Image
#include "primitives/slider.hpp"      // ui::Slider
#include "containers/scrollview.hpp"  // ui::ScrollView
#include "containers/listview.hpp"    // ui::ListView
#include "containers/table.hpp"       // ui::Table
#include "dialog/promptdialog.hpp"    // ui::PromptDialog
#include "window/window.hpp"          // ui::Window (composition root)

// ------------------------------------------------------------
static int PASS = 0, FAIL = 0;
static void check(int cond, const char* what) {
    if (cond) { PASS++; printf("PASS %s\n", what); }
    else      { FAIL++; printf("FAIL %s\n", what); }
}

static uint32_t at(int x, int y) { return g_canvas[y * GW_W + x] & 0xFFFFFFu; }
static int color_eq(color_t a, color_t b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

int main(void) {
    // Tema notepad lewat ABI app: 6 warna color_t (per komponen).
    ui_theme_t abi;
    abi.bg           = COLOR_RGB(0x2D, 0x2D, 0x2D);
    abi.fg           = COLOR_RGB(0xD4, 0xD4, 0xD4);
    abi.accent       = COLOR_RGB(0x00, 0x98, 0xBC);
    abi.button_bg    = COLOR_RGB(0x1E, 0x1E, 0x1E);
    abi.button_fg    = COLOR_RGB(0xD4, 0xD4, 0xD4);
    abi.button_hover = COLOR_RGB(0x3E, 0x3E, 0x42);

    ui_window_t* win = ui_window_create(GW_W, GW_H);
    ui_window_set_theme(win, &abi);
    ui::Window* w = reinterpret_cast<ui::Window*>(win);

    // --- 1. Nilai tema setia + alpha selalu opaque ----------------------
    check(color_eq(w->theme.button_bg, COLOR_RGB(0x1E, 0x1E, 0x1E)) &&
          color_eq(w->theme.bg, COLOR_RGB(0x2D, 0x2D, 0x2D)),
          "tema: nilai RGB sesuai struct ABI");
    check(w->theme.bg.a == 255 && w->theme.fg.a == 255 &&
          w->theme.accent.a == 255 && w->theme.button_fg.a == 255 &&
          w->theme.button_hover.a == 255, "tema: 6 warna ABI semua opaque");
    check(w->theme.editor.a == 255 && w->theme.panel.a == 255,
          "tema: lapisan turunan (derive) opaque");

    // App lama menulis color_t dengan alpha 0 (dulu field-nya uint32 XRGB):
    // alpha itu harus diabaikan, bukan bikin warna tembus.
    ui_theme_t faded = abi;
    faded.bg.a = 0; faded.fg.a = 0;
    ui_window_set_theme(win, &faded);
    check(w->theme.bg.a == 255 && w->theme.fg.a == 255,
          "tema: color_t alpha 0 (app lama) → tetap opaque");
    ui_window_set_theme(win, &abi);

    // to_abi() mengembalikan 6 warna dasar apa adanya.
    ui_theme_t back;
    w->theme.to_abi(&back);
    check(color_eq(back.bg, abi.bg) && color_eq(back.fg, abi.fg) &&
          color_eq(back.accent, abi.accent) && color_eq(back.button_bg, abi.button_bg) &&
          color_eq(back.button_fg, abi.button_fg) && color_eq(back.button_hover, abi.button_hover),
          "tema: to_abi() round-trip 6 warna");
    check(back.bg.a == 255 && back.button_bg.a == 255,
          "tema: to_abi() menulis alpha opaque");

    // --- 2. Gradien tombol: render nyata ke canvas ----------------------
    ui_widget_t* b = ui_button_create(win, "Ganti");
    ui_window_add(win, b);
    w->root->settle();          // layout: posisi tombol dihitung di sini
    w->damage_full();
    w->render();

    ui::Button* btn = reinterpret_cast<ui::Button*>(b);
    check(btn->w > 0 && btn->h > 0, "layout: tombol punya ukuran setelah render");

    int cx = btn->x + btn->w / 2;           // tengah: bebas sudut membulat
    int y0 = btn->y + 1, y1 = btn->y + btn->h - 2;   // +1/-1: lewati border 1px
    uint32_t top = at(cx, y0), bot = at(cx, y1);
    check(top > bot, "tombol: baris atas lebih terang (gradien tidak rata)");
    int distinct = 1;
    for (int iy = y0 + 1; iy <= y1; iy++)
        if (at(cx, iy) != at(cx, iy - 1)) distinct++;
    check(distinct >= 20, "tombol: lerp benar-benar bertahap (>20 warna baris)");

    // Referensi jalur LAMA (sebelum migrasi): aa_shade(#1E1E1E, ±10%) + aa_mix.
    uint32_t base_old = 0x1E1E1E;
    uint32_t old_top = aa_shade(base_old, 10), old_bot = aa_shade(base_old, -10);
    int bad = 0, first_bad = -1;
    for (int iy = y0; iy <= y1; iy++) {
        uint32_t t = (uint32_t)((iy - btn->y) * 255 / (btn->h - 1));
        uint32_t ref = aa_mix(old_bot, old_top, t);
        if (at(cx, iy) != ref) { bad++; if (first_bad < 0) first_bad = iy; }
    }
    if (bad) printf("      baris pertama yang beda: y=%d (dari %d baris)\n", first_bad, y1 - y0 + 1);
    check(bad == 0, "tombol: TIAP baris gradien == aa_shade/aa_mix jalur lama");

    // Nilai ujung yang lama juga harus pas (bukan cuma "ada gradien").
    check(top == aa_mix(old_bot, old_top, (uint32_t)((y0 - btn->y) * 255 / (btn->h - 1))) &&
          bot == aa_mix(old_bot, old_top, (uint32_t)((y1 - btn->y) * 255 / (btn->h - 1))),
          "tombol: ujung gradien sama dengan acuan nilai aa_shade");

    // --- 3. rrect() satu warna tetap rata (tidak ikut bergradien) -------
    ui_widget_t* tb = ui_textbox_create(win, 90);
    ui_window_add(win, tb);
    w->root->settle();
    w->damage_full();
    w->render();
    ui::Widget* tbm = reinterpret_cast<ui::Widget*>(tb);
    int tx = tbm->x + tbm->w / 2;
    check(at(tx, tbm->y + 2) == at(tx, tbm->y + tbm->h - 3),
          "textbox: latar satu warna tetap rata");

    // --- 4. settings.ui: format v1 + migrasi file lama (v0) -------------
    // 4a. save → tag "KTH1" + 6 x color_t (r,g,b,a) = 28 byte.
    check(ui_settings_save(win) == 1, "settings: save berhasil");
    check(g_file_len == 4 + (int)sizeof(ui_theme_t),
          "settings: ukuran file v1 = tag + 6 color_t");
    check(g_file[0] == 'K' && g_file[1] == 'T' && g_file[2] == 'H' && g_file[3] == '1',
          "settings: file v1 diawali tag 'KTH1'");
    check(g_file[4] == 0x2D && g_file[5] == 0x2D && g_file[6] == 0x2D && g_file[7] == 255,
          "settings: payload = 6 x color_t (r,g,b,a=255)");

    // 4b. load file v1 → tema terpasang di window baru.
    ui_window_t* w2 = ui_window_create(GW_W, GW_H);
    check(ui_settings_load(w2) == 1, "settings: load v1 berhasil");
    ui::Window* ww2 = reinterpret_cast<ui::Window*>(w2);
    check(color_eq(ww2->theme.button_bg, COLOR_RGB(0x1E, 0x1E, 0x1E)) &&
          color_eq(ww2->theme.button_hover, COLOR_RGB(0x3E, 0x3E, 0x42)) &&
          ww2->theme.button_bg.a == 255, "settings: tema v1 terpasang utuh");

    // 4c. file LAMA (v0): 6 x uint32 0x00RRGGBB tanpa tag → tetap dibaca.
    static const uint32_t lama[6] = { 0x002D2D2D, 0x00D4D4D4, 0x000098BC,
                                      0x001E1E1E, 0x00D4D4D4, 0x003E3E42 };
    fake_legacy_file(lama, 6);
    ui_window_t* w3 = ui_window_create(GW_W, GW_H);
    check(ui_settings_load(w3) == 1, "settings: file lama (v0) tetap dimuat");
    ui::Window* ww3 = reinterpret_cast<ui::Window*>(w3);
    check(color_eq(ww3->theme.bg, COLOR_RGB(0x2D, 0x2D, 0x2D)) &&
          color_eq(ww3->theme.accent, COLOR_RGB(0x00, 0x98, 0xBC)) &&
          ww3->theme.bg.a == 255, "settings: migrasi v0 → tema terpasang opaque");

    // 4d. file rusak: ukuran tak dikenal → 0, tema tidak berubah.
    ww3->theme.to_abi(&abi);          // acuan sebelum load gagal
    g_file_len = 10;
    check(ui_settings_load(w3) == 0, "settings: ukuran asing ditolak");
    ui_theme_t after;
    ww3->theme.to_abi(&after);
    check(color_eq(after.bg, abi.bg) && color_eq(after.accent, abi.accent),
          "settings: load gagal tidak mengubah tema");

    // 4e. tag salah tapi panjang 28 byte → ditolak (bukan v1, bukan v0).
    ui_settings_save(win);
    g_file[1] = 'X';
    check(ui_settings_load(w3) == 0, "settings: tag salah ditolak");

    // 4f. blob kosong (semua RGB nol) → ditolak, walau tag benar.
    ui_settings_save(win);
    for (int i = 4; i < g_file_len; i++) g_file[i] = 0;
    check(ui_settings_load(w3) == 0, "settings: tema semua-nol ditolak");

    // --- 5. Phase 11 (viewer): auto-fit, center, scroll hanya saat zoom ----
    // Gambar 400x200 di area 200x200 → sumbu tersempit = lebar → fit 50%.
    g_png_w = 400; g_png_h = 200;
    ui_widget_t* sv = ui_scrollview_create(win, 200, 200);
    ui_widget_t* im = ui_image_create(win, "kyuzen.png", 0, 0);
    ui::Image* imm = reinterpret_cast<ui::Image*>(im);
    ui::ScrollView* svm = reinterpret_cast<ui::ScrollView*>(sv);
    ui_scrollview_set_pan(sv, 1);          // mode image-viewer
    ui_scrollview_set_child(sv, im);

    check(imm->iw == 400 && imm->ih == 200, "viewer: PNG termuat (400x200)");
    int nw = 0, nh = 0;
    ui_image_natural_size(im, &nw, &nh);
    check(nw == 400 && nh == 200, "viewer: natural_size melaporkan dimensi PNG");

    int fpct = ui_image_set_fit(im, 200, 200);
    check(fpct == 50 && imm->w == 200 && imm->h == 100,
          "viewer: fit 400x200 ke 200x200 → 50% (200x100)");
    svm->settle();
    check(svm->scroll_max == 0 && svm->hscroll_max == 0,
          "viewer: gambar yang MUAT → tanpa scrollbar sama sekali");
    int ox = -1, oy = -1;
    svm->child_offset(ox, oy);
    check(ox == 0 && oy == 50, "viewer: gambar ditaruh di tengah area (offset 50)");

    // Zoom ke 200% → 800x400: bar dua arah muncul, titik tengah tetap terjaga.
    ui_image_set_scale(im, 200);
    svm->settle();
    check(svm->hscroll_max > 0 && svm->scroll_max > 0,
          "viewer: setelah zoom melebihi area → scrollbar kanan + bawah");
    int vw = svm->w - svm->BAR_W, vh = svm->h - svm->BAR_W;
    // Invarian: tengah gambar == tengah view (scroll = tengah isi − tengah view).
    check(svm->hscroll == imm->w / 2 - vw / 2 && svm->scroll == imm->h / 2 - vh / 2,
          "viewer: zoom menjaga titik tengah (tidak lompat ke pojok kiri-atas)");

    // Kasus portrait: lebar isi < view (jadi di-center) lalu di-zoom → anchor
    // harus benar di sumbu X juga, bukan cuma Y.
    g_png_w = 200; g_png_h = 400;
    ui_widget_t* im3 = ui_image_create(win, "logo.png", 0, 0);
    ui::Image* imm3 = reinterpret_cast<ui::Image*>(im3);
    ui_scrollview_set_child(sv, im3);       // ganti isi scrollview yang sama
    ui_image_set_fit(im3, 200, 200);
    svm->settle();
    svm->child_offset(ox, oy);
    check(imm3->w == 100 && imm3->h == 200 && ox == 50 && oy == 0,
          "viewer: gambar portrait di-center horizontal (bukan nempel kiri)");
    ui_image_set_scale(im3, 300);           // 600x1200, dua-duanya melebihi view
    svm->settle();
    int vx = svm->w - svm->BAR_W, vy = svm->h - svm->BAR_W;
    check(svm->hscroll == imm3->w / 2 - vx / 2 && svm->scroll == imm3->h / 2 - vy / 2,
          "viewer: anchor zoom simetris di sumbu X dan Y");

    // Mode pan OFF (ScrollView app lain) → perilaku lama: kiri-atas, tanpa bar H.
    ui_widget_t* sv2 = ui_scrollview_create(win, 200, 100);
    ui_widget_t* im2 = ui_image_create(win, "kyuzen.png", 0, 0);
    ui::ScrollView* svm2 = reinterpret_cast<ui::ScrollView*>(sv2);
    ui_scrollview_set_child(sv2, im2);
    svm2->settle();
    int ox2 = 0, oy2 = 0;
    svm2->child_offset(ox2, oy2);
    check(svm2->hscroll_max == 0 && ox2 == 0,
          "viewer: ScrollView biasa (tanpa pan) tetap kiri-atas");

    // Sidebar: pilih baris dari kode (viewer dibuka dari Explorer) + auto-gulir.
    ui_widget_t* lv = ui_listview_create(win, 120, 40);   // 2 baris terlihat
    for (int i = 0; i < 5; i++) ui_listview_add_item(lv, "berkas");
    ui_listview_set_selected(lv, 4);
    ui::ListView* lvm = reinterpret_cast<ui::ListView*>(lv);
    check(ui_listview_selected(lv) == 4, "viewer: listview bisa dipilih dari kode");
    check(lvm->scroll > 0, "viewer: baris terpilih digulirkan ke dalam view");
    ui_listview_set_selected(lv, 99);
    check(ui_listview_selected(lv) == 4, "viewer: index di luar rentang diabaikan");

    // --- 6. Slider: lebar <= lebar handle aman dari divide-by-zero -------
    // Dulu clamp_to() menghitung nw = w - 8 lalu membaginya. Widget selebar
    // handle (atau lebih kecil) → nw = 0 → pembagian nol. Di bare-metal tanpa
    // exception itu crash/UB diam-diam, bukan sekadar nilai salah tampil.
    {
        const int widths[4] = { 0, 1, 7, ui::Slider::HANDLE_W };
        const int probes[7] = { -1000, -1, 0, 3, 7, 160, 100000 };
        int ok = 1;
        for (int k = 0; k < 4; k++) {
            ui::Slider s(10, 90);
            s.w = widths[k];
            s.set_value(50);     // nilai awal BUKAN min: biar gerakan palsu terlihat
            for (int j = 0; j < 7; j++) {
                s.clamp_to(probes[j]);
                if (s.val != 50) ok = 0;                      // tak ada ruang gerak → tetap
                if (s.val < s.min || s.val > s.max) ok = 0;    // dan tetap di rentang valid
            }
        }
        check(ok == 1, "slider: lebar <= handle → clamp_to() aman (tanpa divide-by-zero), nilai tetap");

        // Jalur normal tidak ikut berubah: ujung kiri = min, ujung kanan = max,
        // dan klik di luar widget tetap di-clamp (bukan melompat).
        ui::Slider s(0, 100);
        s.x = 40;
        s.w = 160;
        s.clamp_to(s.x);                                // paling kiri
        int lo = s.val;
        s.clamp_to(s.x + s.w - ui::Slider::HANDLE_W);   // paling kanan
        int hi = s.val;
        s.clamp_to(s.x - 500);                          // jauh di kiri
        int lo2 = s.val;
        s.clamp_to(s.x + 5000);                         // jauh di kanan
        int hi2 = s.val;
        check(lo == 0 && hi == 100 && lo2 == 0 && hi2 == 100,
              "slider: jalur normal (kiri=min, kanan=max, luar widget di-clamp)");
    }

    // --- 7. PromptDialog: klik sejajar dengan offset teks di draw() -------
    // draw() menggambar teks di x + 22 (caret di x + 22 + cur*8), tapi
    // input_at() menghitung dari x + 18 → klik/caret meleset 4px (setengah sel)
    // dari teks yang terlihat. Keduanya sekarang pakai PromptDialog::INPUT_PAD_X.
    {
        ui::PromptDialog pd(w, "Simpan", "Nama berkas:", "abcdefghij", 0, 0);
        int ok = 1;
        for (int n = 0; n <= 10; n++) {
            // Batas sel karakter ke-n tepat di bawah huruf yang digambar draw().
            if (pd.input_at(pd.x + ui::PromptDialog::INPUT_PAD_X + n * 8) != n) ok = 0;
        }
        // Klik di paruh kiri sel membulat ke batas kirinya, paruh kanan ke kanan.
        int half_l = pd.input_at(pd.x + ui::PromptDialog::INPUT_PAD_X + 3 * 8 + 3);
        int half_r = pd.input_at(pd.x + ui::PromptDialog::INPUT_PAD_X + 3 * 8 + 4);
        // Klik di luar kolom (kiri dialog) tetap di-clamp ke awal teks.
        int before = pd.input_at(pd.x);
        check(ok == 1 && half_l == 3 && half_r == 4 && before == 0,
              "prompt: klik → indeks kursor sejajar offset teks draw() (x+22)");
    }

    // --- 8. Table: clear() tidak meninggalkan seleksi/hover stale ---------
    // Path refresh Explorer membebaskan semua baris, tapi selected/hover_row
    // tetap menunjuk index yang sudah tak ada → baris hasil refresh berikutnya
    // bisa ter-highlight "selected" padahal user tak pernah memilihnya.
    {
        ui::Table t(240, 100);
        t.x = 0; t.y = 0;
        t.add_column("Nama", 120);
        const char* r1[1] = { "berkas1" };
        const char* r2[1] = { "berkas2" };
        t.add_row(r1, 1);
        t.add_row(r2, 1);
        check(t.nrows == 2, "table: dua baris masuk sebelum clear()");

        // Simulasi klik baris ke-1 (index 1) lalu refresh.
        t.on_content_click(0, t.y + ui::Table::HEADER_H + ui::Table::ROW_H + 2);
        check(t.selected == 1, "table: klik baris mengubah seleksi");
        t.hover_row = 1;
        t.clear();
        check(t.nrows == 0 && t.selected == -1 && t.hover_row == -1,
              "table: clear() mereset selected/hover_row (tidak menunjuk baris mati)");

        // Refresh mengisi baris baru: tak ada yang ter-highlight tanpa klik.
        t.add_row(r1, 1);
        t.add_row(r2, 1);
        check(t.selected == -1 && t.hover_row == -1,
              "table: setelah clear() + add_row, tak ada baris ter-highlight sendiri");
        t.on_content_click(0, t.y + ui::Table::HEADER_H + ui::Table::ROW_H + 2);
        check(t.selected == 1, "table: seleksi lewat klik tetap jalan setelah clear()");
    }

    printf("\n%d PASS, %d FAIL\n", PASS, FAIL);
    return FAIL == 0 ? 0 : 1;
}
