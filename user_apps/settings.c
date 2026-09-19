// user_apps/settings.c — Setelan (Phase 10): tema + info sistem (libui).
//
// Preset tema (Gelap/Terang/Hijau, sama seperti widget_demo) + Simpan/Muat
// (persist ke "settings.ui", reuse Phase 9). Info sistem: RAM, CPU, ukuran
// layar. Muat tema tersimpan saat start.
//
// Build: settings.o + userlib.o + libgui.o + libui.o

#include "userlib.h"
#include "libui.h"
#include "color_utils.h"   // palet + COLOR_RGB_INIT/COLOR_WHITE_INIT (libs/color)

static ui_window_t* g_win;

// Warna ditulis per komponen (libs/color) — urutan field tetap 6 warna ABI
// ui_theme_t: bg, fg, accent, button_bg, button_fg, button_hover.
static const ui_theme_t tema_gelap = {
    COLOR_RGB_INIT(0x12, 0x12, 0x12), COLOR_RGB_INIT(0xE0, 0xE0, 0xE0),
    COLOR_RGB_INIT(0xE9, 0x45, 0x60), COLOR_RGB_INIT(0x0F, 0x34, 0x60),
    COLOR_WHITE_INIT,          COLOR_RGB_INIT(0x2A, 0x4A, 0x7E),
};
static const ui_theme_t tema_terang = {
    COLOR_RGB_INIT(0xF0, 0xF0, 0xF0), COLOR_RGB_INIT(0x22, 0x22, 0x22),
    COLOR_RGB_INIT(0xD3, 0x2F, 0x2F), COLOR_RGB_INIT(0xCF, 0xD8, 0xDC),
    COLOR_RGB_INIT(0x22, 0x22, 0x22), COLOR_RGB_INIT(0x90, 0xA4, 0xAE),
};
static const ui_theme_t tema_hijau = {
    COLOR_RGB_INIT(0x0D, 0x1F, 0x14), COLOR_RGB_INIT(0xDF, 0xF2, 0xE0),
    COLOR_RGB_INIT(0x4C, 0xAF, 0x50), COLOR_RGB_INIT(0x1B, 0x4D, 0x2E),
    COLOR_RGB_INIT(0xE8, 0xF5, 0xE9), COLOR_RGB_INIT(0x2E, 0x7D, 0x46),
};

static void itoa(uint32_t n, char* b) {
    if (n == 0) { b[0] = '0'; b[1] = '\0'; return; }
    char t[16]; int i = 0;
    while (n > 0 && i < 15) { t[i++] = '0' + (n % 10); n /= 10; }
    int j = 0; while (i > 0) b[j++] = t[--i]; b[j] = '\0';
}

static void on_theme(void* userdata) {
    ui_window_set_theme(g_win, (const ui_theme_t*)userdata);
}
static void on_save(void* userdata) { (void)userdata; ui_settings_save(g_win); }
static void on_load(void* userdata) { (void)userdata; ui_settings_load(g_win); }

// --- label dinamis ---
static void label_ram(ui_widget_t* lbl, uint32_t used, uint32_t tot) {
    char b[40]; int k = 0;
    const char* p = "RAM  : ";
    while (p[k]) { b[k] = p[k]; k++; }
    char n[16];
    itoa(used, n); for (int i = 0; n[i] && k < 30; i++) b[k++] = n[i];
    p = " MB / "; for (int i = 0; p[i] && k < 36; i++) b[k++] = p[i];
    itoa(tot, n); for (int i = 0; n[i] && k < 38; i++) b[k++] = n[i];
    b[k++] = ' '; b[k++] = 'M'; b[k++] = 'B';
    b[k] = '\0';
    ui_label_set_text(lbl, b);
}
static void label_cpu(ui_widget_t* lbl) {
    char cpu[49]; get_cpu_string(cpu);
    char b[64]; int k = 0;
    const char* p = "CPU  : ";
    while (p[k]) { b[k] = p[k]; k++; }
    for (int i = 0; cpu[i] && k < 58; i++) b[k++] = cpu[i];
    b[k] = '\0';
    ui_label_set_text(lbl, b);
}
// Phase 2C §9.6 — statistik GPU (syscall 65). Backend software: tampil "-".
static void label_gpu(ui_widget_t* lbl) {
    gpu_stats_t st;
    if (sys_gpu_stats(&st) != 0) {
        ui_label_set_text(lbl, "GPU  : -");
        return;
    }
    char b[88]; int k = 0;
    const char* p = "GPU  : ";
    while (p[k]) { b[k] = p[k]; k++; }
    char n[16];
    itoa((uint32_t)st.present_count, n);
    for (int i = 0; n[i] && k < 78; i++) b[k++] = n[i];
    p = " present / ";
    for (int i = 0; p[i] && k < 78; i++) b[k++] = p[i];
    itoa((uint32_t)st.cmd_count, n);
    for (int i = 0; n[i] && k < 78; i++) b[k++] = n[i];
    p = " cmd / ";
    for (int i = 0; p[i] && k < 78; i++) b[k++] = p[i];
    itoa((uint32_t)st.notify_count, n);
    for (int i = 0; n[i] && k < 78; i++) b[k++] = n[i];
    p = " notify";
    for (int i = 0; p[i] && k < 82; i++) b[k++] = p[i];
    b[k] = '\0';
    ui_label_set_text(lbl, b);
}
static void label_screen(ui_widget_t* lbl, uint32_t w, uint32_t h) {
    char b[32]; int k = 0;
    const char* p = "Layar: ";
    while (p[k]) { b[k] = p[k]; k++; }
    char n[16];
    itoa(w, n); for (int i = 0; n[i] && k < 28; i++) b[k++] = n[i];
    b[k++] = 'x';
    itoa(h, n); for (int i = 0; n[i] && k < 30; i++) b[k++] = n[i];
    b[k] = '\0';
    ui_label_set_text(lbl, b);
}

void main(void) {
    g_win = ui_window_create(340, 400);
    if (!g_win) { sys_exit(); }
    ui_window_set_title(g_win, "Setelan");
    ui_settings_load(g_win);   // pakai tema tersimpan (kalau ada)

    ui_widget_t* sv = ui_scrollview_create(g_win, 320, 350);
    ui_widget_t* box = ui_vbox_create(g_win, 8);

    ui_layout_add(box, ui_label_create(g_win, "Tema"));
    ui_widget_t* b = ui_button_create(g_win, "Tema Gelap");
    ui_button_set_click(b, on_theme, (void*)&tema_gelap);
    ui_layout_add(box, b);
    b = ui_button_create(g_win, "Tema Terang");
    ui_button_set_click(b, on_theme, (void*)&tema_terang);
    ui_layout_add(box, b);
    b = ui_button_create(g_win, "Tema Hijau");
    ui_button_set_click(b, on_theme, (void*)&tema_hijau);
    ui_layout_add(box, b);

    ui_layout_add(box, ui_label_create(g_win, "Setelan (persist settings.ui)"));
    b = ui_button_create(g_win, "Simpan");
    ui_button_set_click(b, on_save, 0);
    ui_layout_add(box, b);
    b = ui_button_create(g_win, "Muat");
    ui_button_set_click(b, on_load, 0);
    ui_layout_add(box, b);

    ui_layout_add(box, ui_label_create(g_win, "Info Sistem"));
    ui_widget_t* ram = ui_label_create(g_win, "RAM  : -");
    ui_layout_add(box, ram);
    ui_widget_t* cpu = ui_label_create(g_win, "CPU  : -");
    ui_layout_add(box, cpu);
    ui_widget_t* scr = ui_label_create(g_win, "Layar: -");
    ui_layout_add(box, scr);
    ui_widget_t* gpu = ui_label_create(g_win, "GPU  : -");
    ui_layout_add(box, gpu);

    ui_scrollview_set_child(sv, box);
    ui_window_add(g_win, sv);

    uint32_t sw = 0, sh = 0;
    sys_get_screen_size(&sw, &sh);
    label_ram(ram, sys_used_ram() / 1024 / 1024, sys_total_ram() / 1024 / 1024);
    label_cpu(cpu);
    label_screen(scr, sw, sh);
    label_gpu(gpu);

    ui_window_run(g_win);   // blocking; keluar via X titlebar / ESC
    ui_window_destroy(g_win);
    sys_exit();
}
