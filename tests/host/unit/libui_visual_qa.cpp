// tests/host/unit/libui_visual_qa.cpp — render nyata libui ke PPM untuk QA.
//
// MENGAPA ADA
// "Terlihat modern" bukan kriteria yang bisa diterima, dan tidak ada test yang
// bisa menilai komposisi visual. Yang BISA dilakukan headless adalah MERENDER
// widget sungguhan ke kanvas lalu menulis gambar — sehingga hasilnya bisa
// diperiksa mata tanpa boot QEMU.
//
// Alat ini memakai widget PRODUKSI yang sama dengan aplikasi (Button, Switch,
// ListView, Section, ...), jadi apa yang terlihat di sini = apa yang dilihat
// pengguna. Beberapa halaman dirender pada 1280x720 dan 1920x1080, di kedua
// mode tema, sesuai checklist QA.
//
// Keluaran: PPM (P6) — format paling sederhana yang bisa ditulis tanpa
// encoder. Konversi ke PNG dilakukan di luar (opsional, bukan dependensi).
//
//   ./test-libui-visual-qa <out_dir>
//
// Tanpa argumen: menulis ke direktori kerja saat ini dengan nama baku.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// CATATAN URUTAN INCLUDE: <string.h> host HARUS lebih dulu. userlib.h
// mendeklarasikan memcpy/memset/strcmp tanpa noexcept, yang konflik dengan
// deklarasi libc++ bila userlib.h masuk duluan (pola yang sama di
// libui_theme_test.cpp dan apps/settings/settings.hpp).
extern "C" {
#include "userlib.h"
#include "libgui.h"
}

// --- stub platform (pola libui_theme_test.cpp) ---
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
void gui_destroy(gui_window_t* win) {
    free(win);
    free(g_canvas);
    g_canvas = 0;
}
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

// PNG: toolkit mereferensikan decoder (primitives/image.cpp + ABI). QA tidak
// memuat gambar apa pun, jadi stub yang selalu gagal decode sudah cukup —
// sama seperti libui_theme_test.cpp.
uint32_t* png_decode(const char* f, int* w, int* h) {
    (void)f;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
}
void png_free(uint32_t* b) { free(b); }
}

#include "core/theme.hpp"
#include "core/painter.hpp"
#include "core/state.hpp"
#include "primitives/button.hpp"
#include "primitives/checkbox.hpp"
#include "primitives/radio.hpp"
#include "primitives/switch.hpp"
#include "primitives/slider.hpp"
#include "primitives/textbox.hpp"
#include "primitives/combobox.hpp"
#include "primitives/progressbar.hpp"
#include "primitives/iconview.hpp"
#include "containers/section.hpp"
#include "containers/listview.hpp"
#include "containers/tab.hpp"
#include "chrome/menu.hpp"
#include "chrome/toolbar.hpp"
#include "chrome/menubar.hpp"
#include "chrome/statusbar.hpp"

namespace {

using ui::Painter;

int g_fail = 0;
void check(int cond, const char* what) {
    if (!cond) { printf("FAIL %s\n", what); g_fail++; }
    else       { printf("PASS %s\n", what); }
}

// Tulis PPM (P6). Sederhana, tanpa dependensi, cukup untuk diperiksa mata.
void write_ppm(const char* path, uint32_t* px, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) { printf("FAIL tidak bisa menulis %s\n", path); g_fail++; return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint32_t v = px[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)((v >> 16) & 0xFF);
        rgb[1] = (unsigned char)((v >> 8) & 0xFF);
        rgb[2] = (unsigned char)(v & 0xFF);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("WROTE %s (%dx%d)\n", path, w, h);
}

// Label teks dirender sebagai BAR (stub gui_draw_text tidak menggambar glyph).
// Untuk QA komposisi ini justru berguna: yang diperiksa adalah ritme, jarak,
// dan state permukaan — bukan bentuk huruf. Tinggi bar = tinggi glyph.
void text_bar(Painter& p, const char* s, int x, int y, int h, color_t c) {
    int n = 0;
    while (s && s[n]) n++;
    p.rect(x, y, n * 8, h, c);
}

// ------------------------------------------------------------
// Halaman QA: satu layar penuh widget dalam komposisi nyata (bukan grid
// komponen terpisah) — supaya jarak antar kontrol ikut terlihat.
// ------------------------------------------------------------
void render_page(const char* out_dir, const char* name, ui_theme_mode_t mode,
                 ui_theme_accent_t accent, int W, int H) {
    ui_theme_config_t cfg;
    cfg.mode = mode;
    cfg.accent = accent;
    cfg.custom = color_hex(0x000000);
    ui::Theme t;
    t.apply_config(&cfg);

    gui_window_t* win = gui_create_window((uint32_t)W, (uint32_t)H);
    ui::Painter p(win, t);

    // Latar halaman.
    p.rect(0, 0, W, H, t.bg);

    // Chrome: menubar + toolbar (pita atas), statusbar (pita bawah).
    ui::MenuBar* mb = new ui::MenuBar(0, W);
    mb->x = 0; mb->y = 0; mb->w = W;
    mb->draw(p);
    ui::Toolbar* tb = new ui::Toolbar(W);
    tb->x = 0; tb->y = mb->h; tb->w = W;
    tb->draw(p);

    int y = mb->h + tb->h + t.metrics.lg;

    // Judul halaman (peran `title`).
    text_bar(p, "Appearance", t.metrics.xl, y, 12, t.text);
    y += t.type.title.bitmap_line_h + t.metrics.sm;

    // --- Section: Controls ---
    ui::Section sec("Controls", t.metrics.sm);
    sec.x = t.metrics.xl; sec.y = y; sec.w = W - 4 * t.metrics.xl;

    ui::Button b1("Primary");
    b1.set_variant(UI_BUTTON_PRIMARY);
    b1.set_icon(UI_ICON_CHECK);
    ui::Button b2("Secondary");
    ui::Button b3("Tertiary");
    b3.set_variant(UI_BUTTON_TERTIARY);
    ui::Button b4("Delete");
    b4.set_variant(UI_BUTTON_DANGER);
    b4.set_icon(UI_ICON_TRASH);
    ui::Button b5("");
    b5.set_icon(UI_ICON_SETTINGS);

    int cy = y + sec.header_h();
    int cx = t.metrics.xl;
    b1.x = cx; b1.y = cy; b1.draw(p); cx += b1.w + t.metrics.sm;
    b2.x = cx; b2.y = cy; b2.draw(p); cx += b2.w + t.metrics.sm;
    b3.x = cx; b3.y = cy; b3.draw(p); cx += b3.w + t.metrics.sm;
    b4.x = cx; b4.y = cy; b4.draw(p); cx += b4.w + t.metrics.sm;
    b5.x = cx; b5.y = cy; b5.draw(p);
    // State: hover / pressed / focused / disabled berjajar supaya perbedaan
    // permukaan bisa dibandingkan langsung.
    cy += b1.h + t.metrics.sm;
    cx = t.metrics.xl;
    ui::Button h1("Hover");   h1.set_hover(true);
    ui::Button h2("Pressed"); h2.set_hover(true); h2.on_click(0, 0);
    ui::Button h3("Focused"); h3.set_focus(true);
    ui::Button h4("Disabled"); h4.set_enabled(false);
    ui::Button* st[4] = {&h1, &h2, &h3, &h4};
    for (int i = 0; i < 4; i++) {
        st[i]->x = cx; st[i]->y = cy; st[i]->draw(p);
        cx += st[i]->w + t.metrics.sm;
    }
    y = cy + h1.h + t.metrics.lg;

    // --- Section: Inputs ---
    ui::Section sec2("Inputs", t.metrics.sm);
    sec2.x = t.metrics.xl; sec2.y = y; sec2.w = W - 4 * t.metrics.xl;
    sec2.draw(p);
    cy = y + sec2.header_h();
    cx = t.metrics.xl;
    ui::TextBox tx1(200); tx1.set_text("Editable");
    tx1.x = cx; tx1.y = cy; tx1.draw(p);
    cx += tx1.w + t.metrics.md;
    ui::TextBox tx2(200); tx2.set_text("invalid"); tx2.set_error(true);
    tx2.x = cx; tx2.y = cy; tx2.draw(p);
    cx += tx2.w + t.metrics.md;
    ui::TextBox tx3(200); tx3.set_text("Focused"); tx3.set_focus(true);
    tx3.x = cx; tx3.y = cy; tx3.draw(p);
    cy += tx1.h + t.metrics.md;
    cx = t.metrics.xl;
    ui::ComboBox cbx; cbx.w = 200; cbx.h = t.metrics.control_h;
    cbx.x = cx; cbx.y = cy; cbx.draw(p);
    cx += cbx.w + t.metrics.md;
    ui::Slider sl(0, 100); sl.set_value(60);
    sl.x = cx; sl.y = cy; sl.w = 240; sl.draw(p);
    cx += sl.w + t.metrics.md;
    ui::ProgressBar pb(160); pb.set_value(65);
    pb.x = cx; pb.y = cy + 6; pb.draw(p);
    y = cy + t.metrics.control_h + t.metrics.lg;

    // --- Section: Selection ---
    ui::Section sec3("Selection", t.metrics.sm);
    sec3.x = t.metrics.xl; sec3.y = y; sec3.w = W - 4 * t.metrics.xl;
    sec3.draw(p);
    cy = y + sec3.header_h();
    cx = t.metrics.xl;
    ui::CheckBox ck1("Checked");   ck1.set_checked(true);
    ui::CheckBox ck2("Unchecked");
    ui::CheckBox ck3("Disabled");  ck3.set_enabled(false);
    ui::CheckBox* cks[3] = {&ck1, &ck2, &ck3};
    for (int i = 0; i < 3; i++) {
        cks[i]->x = cx; cks[i]->y = cy; cks[i]->draw(p);
        cx += cks[i]->w + t.metrics.md;
    }
    cy += ck1.h + t.metrics.sm;
    cx = t.metrics.xl;
    ui::Switch sw1("On");  sw1.set_on(true, false);
    ui::Switch sw2("Off");
    sw1.x = cx; sw1.y = cy; sw1.draw(p);
    cx += sw1.w + t.metrics.md;
    sw2.x = cx; sw2.y = cy; sw2.draw(p);
    cx += sw2.w + t.metrics.md;
    ui::Radio r1("Balanced"); r1.set_selected(true);
    ui::Radio r2("Performance");
    r1.x = cx; r1.y = cy; r1.draw(p);
    cx += r1.w + t.metrics.sm;
    r2.x = cx; r2.y = cy; r2.draw(p);
    y = cy + sw1.h + t.metrics.lg;

    // --- Section: List (baris kaya) ---
    if (y + 140 < H - 40) {
        ui::Section sec4("Rows", t.metrics.sm);
        sec4.x = t.metrics.xl; sec4.y = y; sec4.w = W - 4 * t.metrics.xl;
        sec4.draw(p);
        ui::ListView lv(W - 4 * t.metrics.xl, 130);
        lv.x = t.metrics.xl; lv.y = y + sec4.header_h();
        lv.add_row("Display", "Resolution, scaling, night light",
                   ui::ICON_DISPLAY, true);
        lv.add_row("Appearance", "Theme, accent color, wallpaper",
                   ui::ICON_PALETTE, true);
        lv.add_row("Network", "Wi-Fi, Ethernet, VPN", ui::ICON_NETWORK, true);
        lv.selected = 1;
        lv.draw(p);
    }

    // Statusbar di dasar.
    ui::StatusBar sb;
    sb.x = 0; sb.y = H - t.metrics.statusbar_h;
    sb.w = W; sb.h = t.metrics.statusbar_h;
    sb.draw(p);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%s.ppm", out_dir, name,
             mode == UI_THEME_DARK ? "dark" : "light");
    write_ppm(path, win->canvas, W, H);

    // Verifikasi render benar-benar menghasilkan sesuatu. Tanpa ini, sebuah
    // regresi yang membuat layar kosong (mis. semua widget ter-prune) akan
    // lolos sebagai "render selesai tanpa crash".
    long distinct = 0;
    {
        // Sampling 4px: cukup untuk membedakan "tergambar" dari "kosong".
        uint32_t seen[64];
        int nseen = 0;
        for (int y = 0; y < H && nseen < 64; y += 4) {
            for (int x = 0; x < W && nseen < 64; x += 4) {
                uint32_t v = win->canvas[(size_t)y * W + x] & 0xFFFFFFu;
                bool dup = false;
                for (int k = 0; k < nseen; k++)
                    if (seen[k] == v) { dup = true; break; }
                if (!dup) seen[nseen++] = v;
            }
        }
        distinct = nseen;
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "%s/%s: render punya hierarki (>=12 warna unik)",
             name, mode == UI_THEME_DARK ? "dark" : "light");
    // Halaman QA memuat chrome + 6 kelompok kontrol + daftar: jauh lebih dari
    // 12 warna unik kalau benar-benar tergambar. Ambang rendah supaya tidak
    // rapuh, tapi tetap menangkap "layar kosong".
    check(distinct >= 12, msg);

    delete mb;
    delete tb;
    gui_destroy(win);
}

}  // namespace

int main(int argc, char** argv) {
    const char* out = (argc > 1) ? argv[1] : ".";

    // 1280x720 dan 1920x1080, terang dan gelap — checklist QA.
    render_page(out, "ui_1280x720", UI_THEME_DARK, UI_ACCENT_NEUTRAL, 1280, 720);
    render_page(out, "ui_1280x720", UI_THEME_LIGHT, UI_ACCENT_NEUTRAL, 1280, 720);
    render_page(out, "ui_1920x1080", UI_THEME_DARK, UI_ACCENT_NEUTRAL, 1920, 1080);
    render_page(out, "ui_1920x1080", UI_THEME_LIGHT, UI_ACCENT_NEUTRAL, 1920, 1080);
    // Aksen non-netral: memastikan bahasa visual tidak bergantung pada satu
    // warna aksen (persyaratan eksplisit: biru bukan identitas default).
    render_page(out, "ui_accent", UI_THEME_DARK, UI_ACCENT_PURPLE, 1280, 720);
    render_page(out, "ui_accent", UI_THEME_LIGHT, UI_ACCENT_ORANGE, 1280, 720);

    printf("\nvisual-qa: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
