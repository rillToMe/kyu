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
#include "layout/layout.hpp"          // ui::Layout
#include "layout/vbox.hpp"            // ui::VBox
#include "layout/hbox.hpp"            // ui::HBox
#include "primitives/button.hpp"      // ui::Button
#include "primitives/label.hpp"       // ui::Label
#include "primitives/textbox.hpp"     // ui::TextBox
#include "primitives/checkbox.hpp"    // ui::CheckBox
#include "primitives/radio.hpp"       // ui::Radio
#include "primitives/combobox.hpp"    // ui::ComboBox
#include "primitives/separator.hpp"   // ui::Separator
#include "primitives/progressbar.hpp" // ui::ProgressBar
#include "primitives/image.hpp"       // ui::Image
#include "primitives/slider.hpp"      // ui::Slider
#include "containers/tab.hpp"         // ui::Tab
#include "containers/grid.hpp"        // ui::Grid
#include "containers/scrollview.hpp"  // ui::ScrollView
#include "containers/listview.hpp"    // ui::ListView
#include "containers/table.hpp"       // ui::Table
#include "chrome/menu.hpp"           // ui::Menu
#include "dialog/dialog.hpp"          // ui::Dialog
#include "dialog/promptdialog.hpp"    // ui::PromptDialog
#include "window/window.hpp"          // ui::Window (composition root)

// ------------------------------------------------------------
static int PASS = 0, FAIL = 0;
static void check(int cond, const char* what) {
    if (cond) { PASS++; printf("PASS %s\n", what); }
    else      { FAIL++; printf("FAIL %s\n", what); }
}

static uint32_t at(int x, int y) { return g_canvas[y * GW_W + x] & 0xFFFFFFu; }
// Piksel eksak peran tema (canvas stub selalu opaque seperti libgui asli).
static uint32_t role(color_t c) {
    return color_to_u32(color_opaque(c), FORMAT_ARGB) & 0xFFFFFFu;
}
static void wipe(void) { memset(g_canvas, 0, sizeof(g_canvas)); }
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
    // --- 2. Tombol Phase B: flat + state-based (tanpa gradien) -----------
    // (Menggantikan kunci gradien aa_shade/aa_mix: Phase B sengaja meratakan
    // tombol menjadi flat surface_elevated + border 1px. Regresi yang dikunci
    // sekarang: isi flat, border = border role.)
    ui_widget_t* b = ui_button_create(win, "Ganti");
    ui_window_add(win, b);
    w->root->settle();          // layout: posisi tombol dihitung di sini
    w->damage_full();
    w->render();

    ui::Button* btn = reinterpret_cast<ui::Button*>(b);
    check(btn->w > 0 && btn->h > 0, "layout: tombol punya ukuran setelah render");

    uint32_t elev = color_to_u32(color_opaque(w->theme.surface_elevated), FORMAT_ARGB) & 0xFFFFFFu;
    uint32_t bord = color_to_u32(color_opaque(w->theme.border), FORMAT_ARGB) & 0xFFFFFFu;
    int cx = btn->x + 3;                  // kiri: bebas teks (teks di tengah)
    int y0 = btn->y + 2, y1 = btn->y + btn->h - 3;   // bebas border 1px + sudut
    int flat = 1;
    for (int iy = y0 + 1; iy <= y1; iy++)
        if (at(cx, iy) != at(cx, y0)) flat = 0;
    check(flat, "tombol: isi flat satu warna (tanpa gradien)");
    check(at(cx, y0) == elev, "tombol: isi = surface_elevated");
    check(at(btn->x + btn->w / 2, btn->y) == bord,
          "tombol: border atas = border role");

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

    // --- 9. Menu: relayout() menandai bounds lama DAN baru (anti-ghosting) -
    // Menu lahir 150px; add_item() label pendek me-relayout jadi 130px, jadi
    // menu MENGCIL. Dulu hanya bounds baru yang di-mark → 20px bekas menu di
    // kanan tak pernah digambar ulang dan sisa render lama tertinggal.
    {
        ui::Menu m(w);
        m.x = 10; m.y = 10;
        int old_w = m.w, old_h = m.h;
        m.add_item("OK", 0, 0);              // relayout: w 150 → 130
        int ox = 0, oy = 0, ow = 0, oh = 0;
        bool got = m.take_dirty(ox, oy, ow, oh);
        check(got && m.w < old_w,
              "menu: relayout mengecilkan w (150 → 130) — kasus ghosting");
        check(got && ox <= m.x && oy <= m.y &&
              ox + ow >= m.x + old_w && oy + oh >= m.y + old_h &&
              ox + ow >= m.x + m.w && oy + oh >= m.y + m.h,
              "menu: dirty rect mencakup bounds lama ∪ baru");
    }

    // --- 10. Phase A: mode × aksen → palet semantik ----------------------
    // Default = Dark + Neutral, tanpa biru legacy (0x0F3460/0x2A4A7E).
    {
        ui::Theme def;
        check(color_eq(def.bg, COLOR_HEX(0x111111)) &&
              color_eq(def.text, COLOR_HEX(0xF2F2F2)) &&
              color_eq(def.accent, COLOR_HEX(0x8B8B8B)),
              "config: default = Dark + Neutral (bg/text/accent)");
        int has_blue = 0;
        const color_t scan[] = { def.bg, def.surface, def.surface_elevated,
            def.text, def.border, def.accent, def.accent_hover,
            def.accent_pressed, def.selection, def.focus };
        for (unsigned i = 0; i < sizeof(scan) / sizeof(scan[0]); i++) {
            uint32_t v = color_to_u32(color_opaque(scan[i]), FORMAT_ARGB) & 0xFFFFFFu;
            if (v == 0x0F3460 || v == 0x2A4A7E) has_blue = 1;
        }
        check(!has_blue, "config: default bebas biru legacy 0x0F3460/0x2A4A7E");
        check(!color_eq(def.selection, def.surface) &&
              !color_eq(def.selection, def.bg),
              "config: selection dibedakan dari surface/bg");
        check(color_eq(def.focus, def.accent),
              "config: focus == accent");
    }

    // Matriks 2 mode × 6 aksen: base eksak + turunan konsisten + teks terbaca.
    {
        static const ui_theme_accent_t accs[6] = { UI_ACCENT_NEUTRAL,
            UI_ACCENT_BLUE, UI_ACCENT_PURPLE, UI_ACCENT_GREEN,
            UI_ACCENT_ORANGE, UI_ACCENT_RED };
        static const uint32_t bases[6] = { 0x8B8B8B, 0x2F81F7, 0xA371F7,
            0x3FB950, 0xE8912D, 0xF85149 };
        int ok = 1;
        for (int m = 0; m < 2 && ok; m++) {
            for (int k = 0; k < 6 && ok; k++) {
                ui_theme_config_t cfg;
                cfg.mode = m ? UI_THEME_LIGHT : UI_THEME_DARK;
                cfg.accent = accs[k];
                cfg.custom = COLOR_RGB(0, 0, 0);
                ui_window_t* tw = ui_window_create(64, 64);
                ui_window_set_theme_config(tw, &cfg);
                ui::Window* ww = reinterpret_cast<ui::Window*>(tw);
                color_t want = color_hex(bases[k]);
                if (!color_eq(ww->theme.accent, want)) ok = 0;
                // Turunan hidup: hover/pressed/subtle beda dari base & satu sama lain.
                if (color_eq(ww->theme.accent_hover, ww->theme.accent) ||
                    color_eq(ww->theme.accent_pressed, ww->theme.accent) ||
                    color_eq(ww->theme.accent_hover, ww->theme.accent_pressed) ||
                    color_eq(ww->theme.accent_subtle, ww->theme.accent)) ok = 0;
                // Kontras: teks aksen = pilihan luminance; seleksi terlihat.
                if (!color_eq(ww->theme.accent_contrast,
                              color_get_contrast_text(ww->theme.accent))) ok = 0;
                if (color_eq(ww->theme.selection, ww->theme.surface) ||
                    color_eq(ww->theme.selection, ww->theme.bg)) ok = 0;
                // Teks terbaca di atas background (jarak luma > 100).
                uint32_t lt = (299u * ww->theme.text.r + 587u * ww->theme.text.g +
                               114u * ww->theme.text.b) / 1000u;
                uint32_t lb = (299u * ww->theme.bg.r + 587u * ww->theme.bg.g +
                               114u * ww->theme.bg.b) / 1000u;
                uint32_t d = lt > lb ? lt - lb : lb - lt;
                if (d < 100) ok = 0;
                // Border & teks sekunder menempel di antara bg dan teks.
                if (color_eq(ww->theme.border, ww->theme.bg) ||
                    color_eq(ww->theme.text_secondary, ww->theme.text)) ok = 0;
                // Field legacy terisi dari semantik (arsitektur lama tetap jalan).
                if (!color_eq(ww->theme.button_bg, ww->theme.surface) ||
                    !color_eq(ww->theme.button_hover, ww->theme.surface_elevated) ||
                    !color_eq(ww->theme.button_fg, ww->theme.text)) ok = 0;
                ui_window_destroy(tw);
            }
        }
        check(ok == 1, "config: matriks 2 mode x 6 aksen konsisten");
    }

    // Custom accent: dipakai apa adanya; hitam pekat → fallback netral.
    {
        ui_window_t* tw = ui_window_create(64, 64);
        ui_theme_config_t cfg;
        cfg.mode = UI_THEME_DARK; cfg.accent = UI_ACCENT_CUSTOM;
        cfg.custom = color_hex(0x7C3AED);
        ui_window_set_theme_config(tw, &cfg);
        ui::Window* ww = reinterpret_cast<ui::Window*>(tw);
        check(color_eq(ww->theme.accent, color_hex(0x7C3AED)),
              "config: custom accent dipakai sebagai base");
        check(!color_eq(ww->theme.accent_hover, ww->theme.accent) &&
              color_eq(ww->theme.focus, ww->theme.accent),
              "config: custom menurunkan hover + focus");
        cfg.custom = COLOR_RGB(0, 0, 0);
        ui_window_set_theme_config(tw, &cfg);
        check(color_eq(ww->theme.accent, color_hex(0x8B8B8B)),
              "config: custom hitam pekat → fallback netral");
        // 0 = abaikan, tema tidak berubah.
        ui_window_set_theme_config(tw, 0);
        check(color_eq(ww->theme.accent, color_hex(0x8B8B8B)),
              "config: cfg null diabaikan");
        ui_window_destroy(tw);
    }

    // Jalur legacy memetakan peran 1:1 (piksel-identik untuk app lama).
    {
        ui_window_set_theme(win, &abi);
        check(color_eq(w->theme.surface, abi.button_bg) &&
              color_eq(w->theme.surface_elevated, abi.button_hover) &&
              color_eq(w->theme.selection, abi.button_bg) &&
              color_eq(w->theme.focus, abi.accent) &&
              color_eq(w->theme.text, abi.fg),
              "config: legacy mapping surface/selection/focus/text 1:1");
        check(color_eq(w->theme.text_disabled,
                       color_darken(abi.fg, 139)) &&
              color_eq(w->theme.border, w->theme.mborder) &&
              color_eq(w->theme.border_subtle, w->theme.divider),
              "config: legacy mapping disabled/border 1:1");
    }

    // --- 11. settings.ui v2 (mode + aksen + custom) ----------------------
    {
        ui_window_t* tw = ui_window_create(64, 64);
        ui_theme_config_t cfg;
        cfg.mode = UI_THEME_DARK; cfg.accent = UI_ACCENT_PURPLE;
        cfg.custom = COLOR_RGB(0, 0, 0);
        ui_window_set_theme_config(tw, &cfg);
        check(ui_settings_save(tw) == 1, "settings: save v2 berhasil");
        check(g_file_len == 10, "settings: ukuran file v2 = 10 byte");
        check(g_file[0] == 'K' && g_file[1] == 'T' && g_file[2] == 'H' && g_file[3] == '2',
              "settings: file v2 diawali tag 'KTH2'");
        check(g_file[4] == UI_THEME_DARK && g_file[5] == UI_ACCENT_PURPLE,
              "settings: payload v2 = mode + aksen");

        ui_window_t* tl = ui_window_create(64, 64);
        check(ui_settings_load(tl) == 1, "settings: load v2 berhasil");
        ui::Window* wl = reinterpret_cast<ui::Window*>(tl);
        check(color_eq(wl->theme.accent, color_hex(0xA371F7)) && wl->cfg_valid,
              "settings: tema v2 terpasang (purple) + cfg_valid");
        ui_window_destroy(tl);

        // Enum korup → tolak, tema tidak berubah.
        g_file[5] = 9;
        ui_window_t* tb = ui_window_create(64, 64);
        ui::Window* wb = reinterpret_cast<ui::Window*>(tb);
        wb->theme.to_abi(&abi);
        check(ui_settings_load(tb) == 0, "settings: aksen v2 di luar rentang ditolak");
        ui_theme_t aft;
        wb->theme.to_abi(&aft);
        check(color_eq(aft.bg, abi.bg), "settings: load v2 gagal tidak mengubah tema");
        ui_window_destroy(tb);

        // Ganti config → legacy → save kembali format v1 (bolak-balik aman).
        ui_window_set_theme(tw, &abi);
        check(ui_settings_save(tw) == 1 && g_file_len == 28 &&
              g_file[3] == '1', "settings: kembali legacy → save v1 lagi");
        ui_window_destroy(tw);
    }

    // --- 12. Phase B: render state per widget (dark + light) ------------
    // Menggambar langsung via ui::Painter ke canvas stub (teks no-op, jadi
    // yang diperiksa = fill/border/seleksi, bukan glyph).
    {
        g_win.width = GW_W; g_win.height = GW_H;
        ui::Theme t;   // default Dark + Neutral
        ui::Painter p(&g_win, t);

        // Button secondary: normal/hover/pressed/focused/disabled.
        ui::Button sb("OK");
        sb.x = 10; sb.y = 10;
        int sx = sb.x + 3, sy = sb.y + sb.h / 2;
        wipe(); sb.draw(p);
        check(at(sx, sy) == role(t.surface_elevated), "pb: secondary = elevated");
        sb.set_hover(true); wipe(); sb.draw(p);
        check(at(sx, sy) == role(ui::theme_mix(t.text, t.surface_elevated, 28)) &&
              at(sx, sy) != role(t.surface_elevated), "pb: hover menegas");
        sb.on_click(0, 0); wipe(); sb.draw(p);
        check(at(sx, sy) == role(color_darken(t.surface_elevated, 30)),
              "pb: pressed menggelap");
        sb.on_release(); sb.set_hover(false); sb.set_focus(true);
        wipe(); sb.draw(p);
        check(at(sb.x + sb.w / 2, sb.y) == role(t.focus), "pb: focus ring terlihat");
        sb.set_focus(false); sb.set_enabled(false);
        wipe(); sb.draw(p);
        check(at(sx, sy) == role(t.surface), "pb: disabled = surface");
        sb.set_hover(true); sb.on_click(0, 0);
        check(!sb.pressed && sb.pick(sx, sy) == 0, "pb: disabled tak merespons");
        wipe(); sb.draw(p);
        check(at(sx, sy) == role(t.surface), "pb: disabled stabil saat hover/klik");

        // Button primary + danger + keyboard.
        ui::Button pr("Simpan");
        pr.set_variant(UI_BUTTON_PRIMARY);
        pr.x = 10; pr.y = 44;
        int px = pr.x + 3, py = pr.y + pr.h / 2;
        wipe(); pr.draw(p);
        check(at(px, py) == role(t.accent), "pb: primary = accent");
        pr.set_hover(true); wipe(); pr.draw(p);
        check(at(px, py) == role(t.accent_hover), "pb: primary hover = accent_hover");
        ui::Button dn("Hapus");
        dn.set_variant(UI_BUTTON_DANGER);
        dn.set_variant(99);   // di luar rentang → secondary
        dn.x = 10; dn.y = 78;
        wipe(); dn.draw(p);
        check(at(dn.x + 3, dn.y + dn.h / 2) == role(t.surface_elevated),
              "pb: varian asing → secondary");
        dn.set_variant(UI_BUTTON_DANGER);
        wipe(); dn.draw(p);
        check(at(dn.x + 3, dn.y + dn.h / 2) == role(t.danger), "pb: danger fill");
        int fired = 0;
        ui::Button kb("Go");
        kb.set_click([](void* u) { *(int*)u = 1; }, &fired);
        kb.set_focus(true);
        kb.on_key('\n', 0x1C, 0);
        check(fired == 1, "pb: Enter mengaktifkan tombol fokus");

        // TextBox: border subtle → hover → focus; disabled redup.
        ui::TextBox tbx(80);
        tbx.x = 100; tbx.y = 10;
        wipe(); tbx.draw(p);
        check(at(tbx.x + 40, tbx.y) == role(t.border_subtle), "pb: input border subtle");
        tbx.set_hover(true); wipe(); tbx.draw(p);
        check(at(tbx.x + 40, tbx.y) == role(t.border), "pb: input hover menegas");
        tbx.set_hover(false); tbx.set_focus(true); wipe(); tbx.draw(p);
        check(at(tbx.x + 40, tbx.y) == role(t.focus), "pb: input focus jelas");
        tbx.set_enabled(false); wipe(); tbx.draw(p);
        check(at(tbx.x + 40, tbx.y) == role(t.border_subtle), "pb: input disabled stabil");

        // CheckBox: unchecked → checked (isi aksen + centang kontras).
        ui::CheckBox cbx("Ingat");
        cbx.x = 100; cbx.y = 44;
        wipe(); cbx.draw(p);
        check(at(cbx.x + 2, cbx.y + 2) == role(t.surface), "pb: checkbox kosong = surface");
        cbx.set_checked(true); wipe(); cbx.draw(p);
        check(at(cbx.x + 2, cbx.y + 2) == role(t.accent), "pb: checkbox isi = accent");
        check(at(cbx.x + 2, cbx.y + 6) == role(t.accent_contrast),
              "pb: centang kontras terbaca");
        cbx.set_focus(true); wipe(); cbx.draw(p);
        check(at(cbx.x, cbx.y + 6) == role(t.focus), "pb: checkbox focus terlihat");

        // Slider: thumb aksen, track ber-outline, keyboard, disabled.
        ui::Slider sl(0, 100);
        sl.x = 100; sl.y = 70; sl.w = 100;
        sl.set_value(0);
        int scy = sl.y + sl.h / 2;
        wipe(); sl.draw(p);
        check(at(sl.x + 4, scy) == role(t.accent), "pb: slider thumb = accent");
        check(at(sl.x + 80, scy) == role(t.surface_elevated), "pb: slider track");
        check(at(sl.x + 80, scy - 3) == role(t.border_subtle), "pb: slider outline");
        sl.set_value(50);
        sl.on_key(0, 0x4B, 0);
        check(sl.val == 49, "pb: slider panah kiri -1");
        sl.set_value(0);
        sl.set_enabled(false); wipe(); sl.draw(p);
        check(at(sl.x + 4, scy) == role(t.text_disabled), "pb: slider disabled");

        // ProgressBar: outline + isi aksen (terlihat di Light).
        ui::ProgressBar pb2(100);
        pb2.x = 100; pb2.y = 100; pb2.w = 100; pb2.h = 16;
        pb2.set_value(50);
        wipe(); pb2.draw(p);
        check(at(pb2.x, pb2.y + 8) == role(t.border_subtle), "pb: progress outline");
        check(at(pb2.x + 2, pb2.y + 8) == role(t.accent), "pb: progress isi = accent");

        // Tab: aktif (surface + underline aksen + teks primer) vs
        // inaktif (teks sekunder).
        ui::Tab tab(160, 60);
        tab.x = 10; tab.y = 50;
        tab.add("Satu", new ui::Label("p1"));
        tab.add("Dua", new ui::Label("p2"));
        wipe(); tab.draw(p);
        check(at(tab.x + 40, tab.y + ui::Tab::STRIP_H - 1) == role(t.accent),
              "pb: tab aktif underline aksen");
        check(at(tab.x + 40, tab.y + 4) == role(t.surface), "pb: tab aktif surface");
        check(at(tab.x + 120, tab.y + 4) == role(t.bg), "pb: tab inaktif = bg");
        check(tab.track_hover(tab.x + 120, tab.y + 5), "pb: tab hover terlacak");
        wipe(); tab.draw(p);
        check(at(tab.x + 120, tab.y + 4) == role(t.surface), "pb: tab hover surface");

        // Menu: hover subtle + kotak centang aksen.
        // (Canvas stub 240 lebar: paksa dims penuh — stub menulis stride
        // selebar window, sedangkan at() membaca stride GW_W.)
        ui_window_t* mw = ui_window_create(120, 60);
        g_win.width = GW_W; g_win.height = GW_H;
        ui::Window* mww = reinterpret_cast<ui::Window*>(mw);
        ui::Painter pm(mww->gw, mww->theme);
        ui::Menu mm(mww);
        mm.x = 10; mm.y = 10;
        mm.add_item("Buka", 0, 0);
        mm.set_checked(0, 1);
        mm.track_hover(mm.x + 4, mm.row_y(0) + 2);
        wipe(); mm.draw(pm);
        check(at(mm.x + 2, mm.row_y(0) + 2) == role(mww->theme.surface_elevated),
              "pb: menu hover subtle");
        check(at(mm.x + 10, mm.row_y(0) + 8) == role(mww->theme.accent),
              "pb: menu centang = accent");
        ui_window_destroy(mw);

        // Dialog: divider judul + tombol primer/ sekunder.
        ui_window_t* dw = ui_window_create(200, 120);
        g_win.width = GW_W; g_win.height = GW_H;
        ui::Window* dww = reinterpret_cast<ui::Window*>(dw);
        ui::Painter pd(dww->gw, dww->theme);
        const char* btns[2] = { "OK", "Batal" };
        ui::Dialog dg(dww, "Judul", "Isi", btns, 2, 0, 0);
        dg.x = 10; dg.y = 10;
        wipe(); dg.draw(pd);
        check(at(dg.x + dg.w / 2, dg.y + 32) == role(dww->theme.border_subtle),
              "pb: dialog divider judul");
        check(at(dg.btn_x(0) + 2, dg.btn_row_y() + 2) == role(dww->theme.accent),
              "pb: dialog aksi primer = accent");
        check(at(dg.btn_x(1) + 2, dg.btn_row_y() + 2) == role(dww->theme.btnfill),
              "pb: dialog aksi sekunder = btnfill");
        ui_window_destroy(dw);

        // List selection memakai selection (bukan warna tombol).
        ui::ListView lv(120, 40);
        lv.x = 10; lv.y = 10;
        lv.add_item("a"); lv.add_item("b");
        lv.on_content_click(lv.x + 2, lv.y + 10);
        wipe(); lv.draw(p);
        check(at(lv.x + 2, lv.y + 10) == role(t.selection),
              "pb: list selected = selection");

        // Light + Purple: primer ungu + tombol terang terbaca.
        ui_theme_config_t lc;
        lc.mode = UI_THEME_LIGHT; lc.accent = UI_ACCENT_PURPLE;
        lc.custom = COLOR_RGB(0, 0, 0);
        ui_window_t* lw = ui_window_create(120, 80);
        g_win.width = GW_W; g_win.height = GW_H;
        ui_window_set_theme_config(lw, &lc);
        ui::Window* lww = reinterpret_cast<ui::Window*>(lw);
        ui::Painter pl(lww->gw, lww->theme);
        ui::Button lp("OK");
        lp.set_variant(UI_BUTTON_PRIMARY);
        lp.x = 10; lp.y = 10;
        wipe(); lp.draw(pl);
        check(at(lp.x + 3, lp.y + lp.h / 2) == role(color_hex(0xA371F7)),
              "pb: light+purple primer ungu");
        ui::Button ls("Batal");
        ls.x = 10; ls.y = 44;
        wipe(); ls.draw(pl);
        check(at(ls.x + 3, ls.y + ls.h / 2) == role(color_hex(0xEAEAEA)),
              "pb: light secondary terang terbaca");
        ui_window_destroy(lw);

        // Custom ekstrem tetap usable (kontras dihitung, bukan hilang).
        ui_theme_config_t cc;
        cc.mode = UI_THEME_DARK; cc.accent = UI_ACCENT_CUSTOM;
        cc.custom = color_hex(0xFFFF00);
        ui_window_t* cw = ui_window_create(64, 64);
        g_win.width = GW_W; g_win.height = GW_H;
        ui_window_set_theme_config(cw, &cc);
        ui::Window* cww = reinterpret_cast<ui::Window*>(cw);
        ui::Painter pc(cww->gw, cww->theme);
        ui::Button cb2("OK");
        cb2.set_variant(UI_BUTTON_PRIMARY);
        cb2.x = 5; cb2.y = 5;
        wipe(); cb2.draw(pc);
        check(at(cb2.x + 3, cb2.y + cb2.h / 2) == role(color_hex(0xFFFF00)) &&
              color_eq(cww->theme.accent_contrast, COLOR_RGB(0, 0, 0)),
              "pb: custom kuning usable + teks hitam");
        ui_window_destroy(cw);
    }

    // --- 13. Phase C: traversal fokus (C1–C8, C22) ----------------------
    {
        ui_window_t* fw = ui_window_create(200, 120);
        ui::Window* fww = reinterpret_cast<ui::Window*>(fw);
        g_win.width = GW_W; g_win.height = GW_H;
        ui_widget_t* box = ui_vbox_create(fw, 0);
        ui_widget_t* bA = ui_button_create(fw, "A");
        ui_widget_t* tB = ui_textbox_create(fw, 80);
        ui_widget_t* cC = ui_checkbox_create(fw, "C");
        ui_widget_t* bD = ui_button_create(fw, "D");
        ui_widget_t* lab = ui_label_create(fw, "info");
        ui_layout_add(box, bA); ui_layout_add(box, tB);
        ui_layout_add(box, cC); ui_layout_add(box, bD);
        ui_layout_add(box, lab);
        ui_window_add(fw, box);
        ui::Widget* A = reinterpret_cast<ui::Widget*>(bA);
        ui::Widget* B = reinterpret_cast<ui::Widget*>(tB);
        ui::Widget* C = reinterpret_cast<ui::Widget*>(cC);
        ui::Widget* D = reinterpret_cast<ui::Widget*>(bD);
        ui::Widget* L = reinterpret_cast<ui::Widget*>(lab);
        (void)L;
        fww->focus_traversal(true);
        check(fww->focused == A, "C1: Tab pertama → widget focusable pertama");
        fww->focus_traversal(true);
        check(fww->focused == B, "C2: Tab maju A→B");
        fww->focus_traversal(true);
        check(fww->focused == C, "C2: Tab maju B→C");
        fww->focus_traversal(true);
        check(fww->focused == D, "C2: Tab maju C→D");
        fww->focus_traversal(true);
        check(fww->focused == A, "C4: Tab wrap D→A");
        fww->focus_traversal(false);
        check(fww->focused == D, "C3: Shift+Tab mundur A→D");
        fww->focus_traversal(false);
        check(fww->focused == C, "C3: Shift+Tab mundur D→C");
        // Urutan hanya berisi yang focusable (label dilewati).
        ui::Widget* lst[64];
        check(fww->tab_order(lst) == 4, "C6: non-focusable dilewati (4 stop)");
        // C5/C22: disabled dilewati kedua arah.
        ui_widget_set_enabled(tB, 0);
        fww->set_focus(A);
        fww->focus_traversal(true);
        check(fww->focused == C, "C5: disabled dilewati (A→C)");
        fww->set_focus(D);
        fww->focus_traversal(true);
        check(fww->focused == A, "C22: wrap melewati disabled (D→A)");
        fww->set_focus(C);
        fww->focus_traversal(false);
        check(fww->focused == A, "C22: mundur melewati disabled (C→A)");
        ui_widget_set_enabled(tB, 1);
        // C7: fokus menunjuk widget yang sudah dihancurkan → self-heal.
        fww->focused = reinterpret_cast<ui::Widget*>(0x1234);
        fww->focus_traversal(true);
        check(fww->focused == A, "C7: fokus mati → pulih ke stop pertama");
        // C8: focus ring ter-render.
        fww->set_focus(A);
        fww->damage_full(); fww->render();
        ui::Button* bAr = reinterpret_cast<ui::Button*>(bA);
        check(at(bAr->x + bAr->w / 2, bAr->y) == role(fww->theme.focus),
              "C8: focus ring tombol ter-render");
        ui_window_destroy(fw);
    }

    // --- 14. Phase C: fokus dialog (C9–C12) -------------------------------
    {
        ui_window_t* fw = ui_window_create(200, 120);
        ui::Window* fww = reinterpret_cast<ui::Window*>(fw);
        g_win.width = GW_W; g_win.height = GW_H;
        ui_widget_t* box = ui_vbox_create(fw, 0);
        ui_widget_t* bX = ui_button_create(fw, "X");
        ui_widget_t* tY = ui_textbox_create(fw, 80);
        ui_layout_add(box, bX); ui_layout_add(box, tY);
        ui_window_add(fw, box);
        ui::Widget* X = reinterpret_cast<ui::Widget*>(bX);
        ui::Widget* Y = reinterpret_cast<ui::Widget*>(tY);
        fww->set_focus(X);
        const char* btns[2] = { "OK", "Batal" };
        ui_dialog_show(fw, "T", "B", btns, 2, 0, 0);
        check(fww->dialog != 0 && fww->focused == fww->dialog,
              "C9: dialog dibuka → fokus di dialog");
        fww->focus_traversal(true);
        check(fww->focused == fww->dialog, "C10: traversal terkunci di modal");
        fww->focus_traversal(false);
        check(fww->focused == fww->dialog, "C10: Shift+Tab terkunci di modal");
        ui::Dialog* dg = fww->dialog;
        dg->on_key(0, 0x4D, 0);
        check(dg->hover_btn == 0, "C: dialog panah menggerakkan hover tombol");
        dg->on_key('\n', 0x1C, 0);   // Enter = OK → tutup
        check(fww->dialog == 0 && fww->focused == X,
              "C11: tutup dialog → fokus kembali ke pemilik");
        // C12: pemilik lama invalid (disabled) → fallback stop pertama.
        ui_widget_set_enabled(bX, 0);
        ui_dialog_show(fw, "T", "B", btns, 2, 0, 0);
        fww->close_dialog();
        check(fww->focused == Y, "C12: pemilik invalid → fallback valid");
        check(!fww->owns_widget(reinterpret_cast<ui::Widget*>(0x5678)),
              "C12: owns_widget aman untuk pointer asing");
        ui_widget_set_enabled(bX, 1);
        ui_window_destroy(fw);
    }

    // --- 15. Phase C: TextBox error (C13–C16) ------------------------------
    {
        g_win.width = GW_W; g_win.height = GW_H;
        ui::Theme t;
        ui::Painter p(&g_win, t);
        ui::TextBox tbx(80);
        tbx.x = 10; tbx.y = 10;
        tbx.set_text("hi");
        int ex = tbx.x + 40, ey = tbx.y, eg = tbx.y + 12;
        ui_widget_t* wbox = reinterpret_cast<ui_widget_t*>(&tbx);
        wipe(); tbx.draw(p);
        check(at(ex, ey) == role(t.border_subtle), "C13: error mati = border subtle");
        ui_textbox_set_error(wbox, 1);
        wipe(); tbx.draw(p);
        check(at(ex, ey) == role(t.danger), "C14: error = border danger");
        check(at(ex, eg) == role(ui::theme_mix(t.danger, t.surface, 26)),
              "C14: error = tint danger di background");
        check(at(ex, eg) != role(t.surface), "C22px: error != normal (piksel)");
        tbx.set_focus(true);
        wipe(); tbx.draw(p);
        check(at(ex, ey) == role(t.focus), "C15: error + fokus = focus ring");
        check(at(ex, eg) == role(ui::theme_mix(t.danger, t.surface, 26)),
              "C15: error + fokus = tint tetap terlihat");
        ui_textbox_set_error(wbox, 0);
        tbx.set_focus(false);
        wipe(); tbx.draw(p);
        check(at(ex, ey) == role(t.border_subtle), "C16: error dibersihkan");
    }

    // --- 16. Phase C: disabled Tab/Menu/Scrollbar (C17–C22) ----------------
    {
        g_win.width = GW_W; g_win.height = GW_H;
        ui::Theme t;
        ui::Painter p(&g_win, t);
        // C17: tombol disabled tak fire + tak bisa di-hit.
        int fired = 0;
        ui::Button db("X");
        db.set_click([](void* u) { *(int*)u = 1; }, &fired);
        db.x = 10; db.y = 10;
        db.set_enabled(false);
        db.on_click(0, 0);
        check(fired == 0 && !db.pressed, "C17: tombol disabled tak fire");
        // C18: checkbox disabled tak toggle.
        ui::CheckBox dc("C");
        dc.set_enabled(false);
        dc.on_click(0, 0);
        check(!dc.checked, "C18: checkbox disabled tak toggle");
        // C19: tab item disabled.
        ui::Tab tt(160, 60);
        tt.x = 10; tt.y = 40;
        tt.add("Satu", new ui::Label("p1"));
        tt.add("Dua", new ui::Label("p2"));
        tt.add("Tiga", new ui::Label("p3"));
        ui_tab_set_enabled(reinterpret_cast<ui_widget_t*>(&tt), 1, 0);
        tt.on_click(tt.x + 160 / 3 + 5, tt.y + 5);   // klik judul "Dua"
        check(tt.active == 0, "C19: klik tab disabled diabaikan");
        check(!tt.track_hover(tt.x + 160 / 3 + 5, tt.y + 5) || tt.hover_idx != 1,
              "C19: tab disabled tak di-hover");
        tt.on_key(0, 0x4D, 0);   // Right: 0 → lewati 1 → 2
        check(tt.active == 2, "C19: panah melewati tab disabled");
        tt.on_key(0, 0x4D, 0);   // Right: 2 → lewati 1 → 0
        check(tt.active == 0, "C25: panah tab wrap melewati disabled");
        // C20: item menu disabled.
        ui_window_t* mw = ui_window_create(120, 60);
        g_win.width = GW_W; g_win.height = GW_H;
        ui::Window* mww = reinterpret_cast<ui::Window*>(mw);
        ui::Menu mm(mww);
        mm.x = 10; mm.y = 10;
        int mfire = 0;
        mm.add_item("A", [](void* u) { *(int*)u = 1; }, &mfire);
        mm.add_item("B", [](void* u) { *(int*)u = 2; }, &mfire);
        mm.set_enabled(1, 0);
        check(mm.item_at(mm.x + 4, mm.row_y(1) + 2) == -1,
              "C20: item disabled tak bisa di-hit");
        mm.on_click(mm.x + 4, mm.row_y(1) + 2);
        check(mfire == 0, "C20: item disabled tak fire");
        check(!mm.track_hover(mm.x + 4, mm.row_y(1) + 2),
              "C20: item disabled tak di-hover");
        mm.on_click(mm.x + 4, mm.row_y(0) + 2);
        check(mfire == 1, "C20: item enabled tetap jalan");
        ui_window_destroy(mw);
        // C21: scrollbar = mouse-only, bukan stop fokus; scroll tetap jalan.
        ui::ListView lv(120, 40);
        check(!lv.focusable(), "C21: list/scrollbar bukan stop fokus");
        for (int i = 0; i < 5; i++) lv.add_item("r");
        int s0 = lv.scroll;
        check(lv.on_scroll(1) && lv.scroll != s0, "C21: scroll roda tetap jalan");
    }

    // --- 17. Phase C: aktivasi keyboard (C23–C24) --------------------------
    {
        int f1 = 0, f2 = 0;
        ui::Button kb("Go");
        kb.set_click([](void* u) { *(int*)u = 1; }, &f1);
        kb.set_focus(true);
        kb.on_key(' ', 0, 0);
        check(f1 == 1, "C23: Spasi mengaktifkan tombol fokus");
        ui::CheckBox kc("K");
        kc.set_focus(true);
        kc.on_key('\r', 0x1C, 0);
        check(kc.checked, "C24: Enter toggle checkbox fokus");
        kc.on_key(' ', 0, 0);
        check(!kc.checked, "C24: Spasi toggle balik");
        (void)f2;
    }

    // --- 18. Phase D: Radio (D1–D7) --------------------------------------
    {
        ui::RadioGroup g;
        ui::Radio a("A"), b("B"), c("C");
        check(!a.is_selected() && a.w > 0 && a.h == 20, "D1: radio lahir unselected");
        a.set_group(&g); b.set_group(&g); c.set_group(&g);
        a.set_selected(true);
        check(a.is_selected() && !b.is_selected() && !c.is_selected(),
              "D2: select diam (tanpa callback)");
        int f = 0;
        b.set_change([](void* u) { *(int*)u = 1; }, &f);
        b.on_click(0, 0);
        check(b.is_selected() && !a.is_selected() && f == 1,
              "D3: eksklusivitas grup + fire sekali");
        f = 0; b.on_click(0, 0);
        check(f == 0, "D3: klik yang terpilih tak fire ulang");
        // D4: disabled tak bisa dipilih (klik maupun select()).
        b.set_enabled(false);
        check(!b.focusable(), "D4: disabled tak focusable");
        c.on_click(0, 0);
        check(c.is_selected(), "D4: setup pilih C");
        b.on_click(0, 0);
        check(c.is_selected() && !b.is_selected(), "D4: klik disabled diabaikan");
        g.select(&b, false);
        check(c.is_selected(), "D4: select() lewati disabled");
        b.set_enabled(true);
        check(b.focusable(), "D4: re-enable kembali focusable");
        // D6: panah pindah + pilih (lewati disabled), Enter pilih.
        ui::Radio d("D");
        d.set_group(&g);
        c.set_enabled(false);
        a.on_key(0, 0x4D, 0);   // Right dari A: B enabled → pilih B
        check(b.is_selected(), "D6: panah kanan pindah + pilih");
        b.on_key(0, 0x4D, 0);   // Right dari B: C disabled → lewati → D
        check(d.is_selected(), "D6: panah lewati disabled");
        d.on_key(0, 0x4D, 0);   // wrap D → A
        check(a.is_selected(), "D6: panah wrap");
        c.set_enabled(true);
        a.on_key('\n', 0x1C, 0);
        check(a.is_selected(), "D6: Enter pilih yang fokus");
        // D7: destruksi dua arah.
        ui::Radio* px = new ui::Radio("P");
        ui::RadioGroup* pg = new ui::RadioGroup();
        px->set_group(pg);
        px->set_selected(true);
        delete pg;
        check(!px->is_selected() && px->group == 0,
              "D7: grup hancur → anggota lepas aman");
        px->on_click(0, 0);
        check(px->is_selected(), "D7: mandiri tetap bisa dipilih");
        delete px;
        ui::RadioGroup* pg2 = new ui::RadioGroup();
        ui::Radio* pa = new ui::Radio("A");
        ui::Radio* pb2 = new ui::Radio("B");
        pa->set_group(pg2); pb2->set_group(pg2);
        pa->set_selected(true);
        delete pa;
        check(pg2->selected == 0 && pg2->n == 1,
              "D7: anggota terpilih dihapus → grup konsisten");
        delete pb2; delete pg2;
        // D5: fokus + traversal (radio = stop traversal biasa).
        check(a.focusable(), "D5: radio focusable");
        // D7-ABI: get_selected.
        ui_radio_group_t* gg = ui_radio_group_create();
        ui_widget_t* r1 = ui_radio_create(0, "R1");
        ui_widget_t* r2 = ui_radio_create(0, "R2");
        ui_radio_set_group(r1, gg);
        ui_radio_set_group(r2, gg);
        ui_radio_set_selected(r1, 1);
        check(ui_radio_get_selected(gg) == r1, "D2-ABI: get_selected");
        check(ui_radio_get_selected(0) == 0, "D2-ABI: grup null aman");
        ui_radio_group_destroy(gg);
        (void)r2;
    }

    // --- 19. Phase D: ComboBox (D8–D16) -----------------------------------
    {
        ui_window_t* cw = ui_window_create(200, 120);
        g_win.width = GW_W; g_win.height = GW_H;
        ui::Window* cww = reinterpret_cast<ui::Window*>(cw);
        ui::ComboBox cb;
        cb.set_owner(cww);
        cb.x = 10; cb.y = 10; cb.w = 120;
        check(cb.selected == -1 && !cb.open && cb.count() == 0, "D8: lahir kosong tertutup");
        check(cb.add_item("Merah") == 0 && cb.add_item("Hijau") == 1 &&
              cb.add_item("Biru") == 2 && cb.count() == 3, "D9: add_item");
        check(cb.remove_item(9) == 0 && cb.remove_item(1) == 1 && cb.count() == 2,
              "D9: remove_item (invalid ditolak)");
        cb.clear();
        check(cb.count() == 0 && cb.selected == -1, "D9: clear");
        cb.add_item("Satu"); cb.add_item("Dua"); cb.add_item("Tiga");
        int cf = 0;
        cb.set_change([](void* u) { *(int*)u += 1; }, &cf);
        cb.set_selected(1);
        check(cb.selected == 1 && cf == 0, "D10: set_selected diam");
        cb.set_selected(9);
        check(cb.selected == 1, "D10: index invalid diabaikan");
        // D11: buka/tutup + pilih via popup.
        cb.on_click(0, 0);
        check(cb.open && cww->popup == cb.menu, "D11: klik membuka popup");
        cb.menu->on_click(cb.menu->x + 4, cb.menu->row_y(2) + 2);
        check(cb.selected == 2 && cf == 1 && !cb.open && cww->popup == 0,
              "D11: pilih item menutup + fire sekali");
        cb.on_click(0, 0);
        check(cb.open, "D11: buka lagi");
        cb.on_click(0, 0);
        check(!cb.open, "D11: klik saat terbuka menutup");
        // D12: keyboard.
        // D12: keyboard (Down = 0x50 set-1; 0x4D adalah Right).
        cb.on_key(0, 0x50, 0);   // Down tertutup: seleksi langsung 2→clamp
        check(cb.selected == 2, "D12: Down di ujung clamp");
        cb.set_selected(0);
        cb.on_key(0, 0x50, 0);
        check(cb.selected == 1 && cf == 2, "D12: Down navigasi + fire");
        cb.on_key('\n', 0x1C, 0);   // Enter: buka
        check(cb.open, "D12: Enter membuka");
        cb.on_key(0, 0x50, 0);      // Down: hover 1→2
        check(cb.menu->hover_idx == 2, "D12: Down gerakkan hover popup");
        cb.on_key('\n', 0x1C, 0);   // Enter: commit
        check(cb.selected == 2 && cf == 3 && !cb.open, "D12: Enter commit + tutup");
        // D13/D14: batal via jalur tutup Window (ESC/klik-luar/Tab sama).
        cb.on_click(0, 0);
        cww->close_popup();
        check(!cb.open && cb.selected == 2, "D13: batal tak ubah seleksi");
        // D15: disabled.
        cb.set_enabled(false);
        check(!cb.focusable(), "D15: disabled tak focusable");
        cb.on_click(0, 0);
        check(!cb.open, "D15: disabled tak membuka");
        cb.set_enabled(true);
        // Piksel: kotak + panah.
        ui::Painter pc(cww->gw, cww->theme);
        wipe(); cb.draw(pc);
        check(at(cb.x + 2, cb.y + 12) == role(cww->theme.surface),
              "D: combo box = surface");
        check(at(cb.x + cb.w - 10, cb.y + 12) == role(cww->theme.text_secondary),
              "D: combo panah sekunder");
        // D16: hancur saat popup terbuka → popup tertutup, tak dangling.
        ui::ComboBox* hp = new ui::ComboBox();
        hp->set_owner(cww);
        hp->x = 10; hp->y = 50; hp->w = 100;
        hp->add_item("x");
        hp->on_click(0, 0);
        check(cww->popup != 0, "D16: setup popup terbuka");
        delete hp;
        check(cww->popup == 0, "D16: hancur → popup tertutup aman");
        // ABI: add/selected/get.
        ui_widget_t* ab = ui_combobox_create(0, 100);
        check(ui_combobox_add_item(ab, "a") == 0 && ui_combobox_count(ab) == 1,
              "D-ABI: create/add/count");
        ui_combobox_set_selected(ab, 0);
        check(ui_combobox_selected(ab) == 0, "D-ABI: set/get selected");
        check(ui_combobox_remove_item(ab, 0) == 1 && ui_combobox_count(ab) == 0,
              "D-ABI: remove/clear implisit");
        ui_combobox_clear(ab);
        ui_window_destroy(cw);
        (void)ab;
    }

    // --- 20. Phase D: Tooltip (D17–D22) -----------------------------------
    {
        ui_window_t* tw = ui_window_create(200, 120);
        ui::Window* tww = reinterpret_cast<ui::Window*>(tw);
        g_win.width = GW_W; g_win.height = GW_H;
        ui_widget_t* box = ui_vbox_create(tw, 0);
        ui_widget_t* bb = ui_button_create(tw, "Tip");
        ui_widget_set_tooltip(bb, "tip!");
        ui_layout_add(box, bb);
        ui_window_add(tw, box);
        ui::Widget* B = reinterpret_cast<ui::Widget*>(bb);
        tww->root->settle();
        reinterpret_cast<ui::Widget*>(box)->settle();   // VBox anak: arrange bertingkat
        // D17: tampil setelah delay, tidak sebelumnya.
        tww->hovered = B;
        tww->tip_since = 1000;
        check(!tww->tip_tick(1599), "D17: sebelum delay tak tampil");
        check(tww->tip_tick(1600) && tww->tip_shown, "D17: tampil setelah 600ms");
        // Piksel tooltip: bg elevated + border.
        ui::Painter pt(tww->gw, tww->theme);
        int tx, ty, twd, th;
        ui::Window::tip_calc(B, 4, GW_W, GW_H, &tx, &ty, &twd, &th);
        tww->damage_full(); tww->render();
        check(at(tx + 2, ty + 10) == role(tww->theme.surface_elevated),
              "D17: tooltip bg elevated");
        check(at(tx, ty + 10) == role(tww->theme.border), "D17: tooltip border");
        // D18: hover pindah → sembunyi.
        tww->hovered = 0;
        check(tww->tip_tick(9999) && !tww->tip_shown, "D18: pointer keluar → hilang");
        // D19/D20: positioning murni.
        ui::Label fl("t");
        ui::Widget* fake = &fl;
        fake->x = 10; fake->y = 50; fake->w = 40; fake->h = 20;
        ui::Window::tip_calc(fake, 4, 240, 120, &tx, &ty, &twd, &th);
        check(ty + th <= 50 && tx == 10, "D19: di atas target");
        fake->y = 2;
        ui::Window::tip_calc(fake, 4, 240, 120, &tx, &ty, &twd, &th);
        check(ty >= fake->y + fake->h, "D19: fallback di bawah");
        fake->x = 230; fake->y = 50;
        ui::Window::tip_calc(fake, 4, 240, 120, &tx, &ty, &twd, &th);
        check(tx + twd <= 240 && tx >= 0, "D20: clamp horizontal");
        fake->x = 0; fake->y = 110; fake->w = 240; fake->h = 20;
        ui::Window::tip_calc(fake, 20, 240, 120, &tx, &ty, &twd, &th);
        check(ty + th <= 120 && ty >= 0, "D20: clamp vertikal jendela kecil");
        // D21: tooltip bukan stop (tanpa handle, traversal tetap).
        ui::Widget* lst[64];
        int before = tww->tab_order(lst);
        ui_widget_set_tooltip(bb, "tip lain");
        check(tww->tab_order(lst) == before, "D21: tooltip tak masuk traversal");
        // D22: target hancur → sembunyi tanpa crash.
        tww->hovered = B;
        tww->tip_since = 0;
        tww->tip_tick(9999);
        tww->tip_target = reinterpret_cast<ui::Widget*>(0x9ABC);
        tww->tip_shown = true;
        check(tww->tip_tick(99999) && !tww->tip_shown, "D22: target mati → hilang aman");
        tww->hovered = 0;
        ui_window_destroy(tw);
    }

    // --- 21. Phase D: Separator (D23–D26) ---------------------------------
    {
        g_win.width = GW_W; g_win.height = GW_H;
        ui::Theme t;
        ui::Painter p(&g_win, t);
        ui::Separator h(false), v(true);
        h.x = 10; h.y = 10; h.w = 100; h.h = 1;
        v.x = 10; v.y = 20; v.w = 1; v.h = 40;
        wipe(); h.draw(p); v.draw(p);
        check(at(60, 10) == role(t.border_subtle), "D23: horizontal 1px");
        check(at(10, 40) == role(t.border_subtle), "D24: vertikal 1px");
        check(!h.focusable() && h.pick(60, 10) == 0, "D25: non-interaktif");
        ui_widget_t* w1 = ui_separator_create(0, UI_SEP_HORIZONTAL);
        ui_widget_t* w2 = ui_separator_create(0, 99);
        check(w1 != 0 && w2 != 0, "D-ABI: create (orientasi asing = horizontal)");
        // D26: partisipasi layout (ukuran caller, posisi flow).
        ui_window_t* sw = ui_window_create(200, 120);
        ui::Window* sww = reinterpret_cast<ui::Window*>(sw);
        ui_widget_t* box = ui_vbox_create(sw, 8);
        ui_widget_t* l1 = ui_label_create(sw, "a");
        ui_widget_t* sp = ui_separator_create(sw, UI_SEP_HORIZONTAL);
        ui_widget_set_size(sp, 100, 1);
        ui_widget_t* l2 = ui_label_create(sw, "b");
        ui_layout_add(box, l1); ui_layout_add(box, sp); ui_layout_add(box, l2);
        ui_window_add(sw, box);
        sww->root->settle();
        reinterpret_cast<ui::Widget*>(box)->settle();   // arrange bertingkat
        ui::Widget* S = reinterpret_cast<ui::Widget*>(sp);
        ui::Widget* L2 = reinterpret_cast<ui::Widget*>(l2);
        check(S->w == 100 && S->h == 1 && L2->y == S->y + 1 + 8,
              "D26: separator di flow VBox");
        ui_window_destroy(sw);
    }

    // --- 22. Phase D: layout (D27–D44) ------------------------------------
    // CATATAN ownership: Layout/Grid MENGHAPUS anaknya (konvensi toolkit) —
    // anak harus heap (pola aplikasi ui_*_create), bukan stack.
    {
        // D27: VBox mengukur dari isi (termasuk trailing spacing existing).
        ui::VBox* vb = new ui::VBox(8);
        vb->x = 5; vb->y = 5;
        ui::Button* v1 = new ui::Button("a");
        ui::Button* v2 = new ui::Button("bb");
        v1->w = 10; v1->h = 10; v2->w = 20; v2->h = 30;
        vb->add(v1); vb->add(v2);
        vb->arrange();
        check(v1->x == 5 && v1->y == 5 && v2->x == 5 && v2->y == 23,
              "D27: VBox posisi anak (spacing 8)");
        check(vb->h == 10 + 8 + 30 + 8, "D27: VBox h = isi + trailing spacing");
        // D28: HBox.
        ui::HBox* hb = new ui::HBox(8);
        hb->x = 5; hb->y = 5;
        ui::Button* h1 = new ui::Button("a");
        ui::Button* h2 = new ui::Button("bb");
        h1->w = 10; h1->h = 10; h2->w = 20; h2->h = 30;
        hb->add(h1); hb->add(h2);
        hb->arrange();
        check(h1->x == 5 && h2->x == 23 && hb->w == 10 + 8 + 20 + 8 && hb->h == 30,
              "D28: HBox posisi + ukuran");
        // D40: hidden dilewati (tak makan tempat).
        v2->set_visible(false);
        vb->arrange();
        check(vb->h == 10 + 8, "D40: hidden tak makan tempat");
        v2->set_visible(true);
        // D41: disabled tetap makan tempat.
        v2->set_enabled(false);
        vb->arrange();
        check(v2->y == 23 && vb->h == 10 + 8 + 30 + 8, "D41: disabled tetap layout");
        delete vb; delete hb;   // hapus anak heap (uji ownership)

        // D35: Grid 2x2 geometri eksak (align START agar ukuran utuh).
        ui::Grid* gr = new ui::Grid(2, 2, 8);
        gr->x = 10; gr->y = 20; gr->w = 216; gr->h = 100;
        gr->set_align(UI_ALIGN_START);
        ui::Button* gA = new ui::Button("a");
        ui::Button* gB = new ui::Button("b");
        ui::Button* gC = new ui::Button("c");
        ui::Button* gD = new ui::Button("d");
        gA->w = 10; gA->h = 10; gB->w = 10; gB->h = 10;
        gC->w = 10; gC->h = 10; gD->w = 10; gD->h = 10;
        check(gr->put(gA, 0, 0, 1, 1) == 1 && gr->put(gB, 0, 1, 1, 1) == 1 &&
              gr->put(gC, 1, 0, 1, 1) == 1 && gr->put(gD, 1, 1, 1, 1) == 1,
              "D35: put 2x2");
        check(gr->put(gA, 0, 0, 1, 1) == 0 && gr->put(gA, 9, 0, 1, 1) == 0,
              "D35: overlap + invalid ditolak");
        gr->arrange();
        check(gA->x == 10 && gA->y == 20 && gB->x == 28 && gB->y == 20 &&
              gC->x == 10 && gC->y == 38 && gD->x == 28 && gD->y == 38,
              "D35: Grid 2x2 posisi (gap 8)");
        // D30/D38: gap hanya antar track (tanpa tepi).
        check(gB->x - (gA->x + gA->w) == 8 && gC->y - (gA->y + gA->h) == 8,
              "D30: gap antar track, tanpa tepi");
        // D33: auto = isi terbesar.
        gB->w = 30;
        gr->arrange();
        check(gB->x == 28 && gD->x == 28 && gB->w == 30,
              "D33: kolom auto mengikuti isi");
        // D32: fixed mengunci (terlihat saat stretch).
        gr->set_col(1, UI_TRACK_FIXED, 100);
        gr->set_align(UI_ALIGN_STRETCH);
        gr->arrange();
        check(gD->w == 100 && gD->x == 28, "D32: kolom fixed 100");
        gr->set_col(1, UI_TRACK_AUTO, 0);
        gr->set_align(UI_ALIGN_START);
        // D37: padding menggeser origin.
        gr->set_padding(4, 0, 0, 0);
        gr->arrange();
        check(gA->x == 14, "D37: padding kiri menggeser");
        gr->set_padding(0, 0, 0, 0);
        // D31: alignment dalam sel (kolom/baris fixed agar sel > isi).
        gr->set_col(0, UI_TRACK_FIXED, 40);
        gr->set_row(0, UI_TRACK_FIXED, 30);
        gr->set_align(UI_ALIGN_END);
        gr->arrange();
        check(gA->x == 40 && gA->y == 40, "D31: END menempel kanan-bawah sel");
        gr->set_align(UI_ALIGN_CENTER);
        gr->arrange();
        check(gA->x == 25 && gA->y == 30, "D31: CENTER tengah sel");
        gr->set_align(UI_ALIGN_STRETCH);
        gr->arrange();
        check(gA->w == 40 && gA->h == 30, "D31: STRETCH mengisi sel");
        gA->w = 10; gA->h = 10;
        gr->set_col(0, UI_TRACK_AUTO, 0);
        gr->set_row(0, UI_TRACK_AUTO, 0);
        gr->set_align(UI_ALIGN_STRETCH);
        // STRETCH memutasi ukuran anak (terdefinisi: isi sel); kembalikan
        // ukuran konten agar AUTO mengukur konten segar di bawah.
        gB->w = 10; gB->h = 10; gC->w = 10; gC->h = 10; gD->w = 10; gD->h = 10;
        // D34/D39: fill membagi sisa + resize.
        gr->set_col(1, UI_TRACK_FILL, 0);
        gr->w = 216;
        gr->arrange();
        check(gD->x == 10 + 10 + 8 && gD->w == 216 - 10 - 8,
              "D34: fill memakai sisa lebar");
        gr->w = 116;
        gr->arrange();
        check(gD->w == 116 - 10 - 8, "D39: resize mendistribusi ulang fill");
        gr->set_align(UI_ALIGN_START);
        delete gr;   // hapus anak heap (uji ownership Grid)
        // D36: kolom tak sama (auto per isi).
        ui::Grid* gu = new ui::Grid(1, 2, 0);
        gu->x = 0; gu->y = 0; gu->w = 200; gu->h = 20;
        ui::Button* u1 = new ui::Button("a");
        ui::Button* u2 = new ui::Button("b");
        u1->w = 15; u1->h = 10; u2->w = 45; u2->h = 10;
        gu->put(u1, 0, 0, 1, 1); gu->put(u2, 0, 1, 1, 1);
        gu->arrange();
        check(u1->x == 0 && u2->x == 15, "D36: kolom auto tak sama");
        delete gu;
        // D42: nested (Grid dalam VBox).
        ui::VBox* outer = new ui::VBox(0);
        outer->x = 0; outer->y = 0;
        ui::Grid* inner = new ui::Grid(1, 1, 0);
        inner->w = 50; inner->h = 20;
        ui::Button* nb = new ui::Button("n");
        nb->w = 10; nb->h = 10;
        inner->put(nb, 0, 0, 1, 1);
        inner->set_align(UI_ALIGN_START);
        outer->add(inner);
        outer->arrange();
        check(nb->x == 0 && nb->y == 0, "D42: nested grid posisi benar");
        delete outer;   // hapus grid + tombol bertingkat
        // D43/D44: span (klamp + kembangkan track).
        ui::Grid* gs = new ui::Grid(2, 2, 8);
        gs->x = 0; gs->y = 0; gs->w = 200; gs->h = 60;
        gs->set_align(UI_ALIGN_START);
        ui::Button* s1 = new ui::Button("s");
        ui::Button* s2 = new ui::Button("t");
        s1->w = 50; s1->h = 10; s2->w = 10; s2->h = 34;
        check(gs->put(s1, 0, 0, 1, 2) == 1, "D43: span ditempatkan");
        check(gs->put(s2, 0, 0, 1, 1) == 0, "D43: overlap span ditolak");
        int mwc[8], mhc[8];
        gs->measure_tracks(mwc, mhc);
        check(mwc[0] == 0 && mwc[1] == 42, "D43: track mengembang untuk span");
        gs->arrange();
        check(s1->x == 0 && s1->y == 0, "D43: span posisi benar");
        delete gs;   // s2 tak pernah terpasang → hapus manual
        delete s2;
        ui::Grid* gs2 = new ui::Grid(2, 2, 8);
        gs2->x = 0; gs2->y = 0; gs2->w = 200; gs2->h = 60;
        ui::Button* t1 = new ui::Button("t");
        t1->w = 10; t1->h = 34;
        check(gs2->put(t1, 0, 0, 5, 5) == 1 &&
              gs2->cells[0].rs == 2 && gs2->cells[0].cs == 2,
              "D44: span dijepit muat");
        delete gs2;
        // ABI put_span.
        ui_widget_t* gab = ui_grid_create(0, 2, 2, 8);
        ui_widget_t* chb = ui_button_create(0, "x");
        check(ui_grid_put(gab, chb, 0, 0) == 1, "D-ABI: put");
        check(ui_grid_put(gab, chb, 0, 0) == 0, "D-ABI: overlap ditolak");
        check(ui_grid_put_span(gab, chb, 1, 1, 9, 9) == 1, "D-ABI: put_span klamp");
        ui_grid_set_col(gab, 0, UI_TRACK_FIXED, 100);
        ui_grid_set_row(gab, 9, UI_TRACK_FIXED, 100);   // invalid diam
        ui_padding_t pd;
        pd.left = 2; pd.top = 2; pd.right = 2; pd.bottom = 2;
        ui_grid_set_padding(gab, pd);
        ui_grid_set_align(gab, UI_ALIGN_CENTER);
        ui_grid_set_align(gab, 99);   // invalid → stretch, diam
    }

    // --- 23. Phase D: matriks tema radio (spot) ------------------------------
    {
        g_win.width = GW_W; g_win.height = GW_H;
        ui_theme_config_t pc;
        pc.mode = UI_THEME_DARK; pc.accent = UI_ACCENT_PURPLE;
        pc.custom = COLOR_RGB(0, 0, 0);
        ui::Theme tp;
        tp.apply_config(&pc);
        ui::Painter pp(&g_win, tp);
        ui::Radio rp("R");
        rp.x = 10; rp.y = 10;
        rp.set_selected(true);
        wipe(); rp.draw(pp);
        check(at(15, 15) == role(color_hex(0xA371F7)), "D: dark+purple dot aksen");
        pc.mode = UI_THEME_LIGHT; pc.accent = UI_ACCENT_NEUTRAL;
        ui::Theme tl;
        tl.apply_config(&pc);
        ui::Painter pl(&g_win, tl);
        ui::Radio rl("R");
        rl.x = 10; rl.y = 40;
        wipe(); rl.draw(pl);
        check(at(10, 45) == role(color_hex(0xD6D6D6)), "D: light ring border");
        pc.mode = UI_THEME_DARK; pc.accent = UI_ACCENT_GREEN;
        ui::Theme tg;
        tg.apply_config(&pc);
        ui::Painter pg(&g_win, tg);
        ui::Radio rg("R");
        rg.x = 10; rg.y = 70;
        rg.set_selected(true);
        wipe(); rg.draw(pg);
        check(at(15, 75) == role(color_hex(0x3FB950)), "D: green dot (aksen ekstra)");
    }

    printf("\n%d PASS, %d FAIL\n", PASS, FAIL);
    return FAIL == 0 ? 0 : 1;
}
