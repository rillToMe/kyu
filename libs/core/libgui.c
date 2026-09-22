// libs/core/libgui.c — Kyuzen GUI Framework Implementation
//
// PENTING: FONT8x16_IMPLEMENTATION hanya boleh di-define SATU kali.
// Karena libgui.c mengimplementasikannya, app yang link libgui.o
// TIDAK BOLEH mendefinisikan FONT8x16_IMPLEMENTATION sendiri.

#define FONT8x16_IMPLEMENTATION
#include "font8x16.h"
#include "libgui.h"
#include "userlib.h"
#include "color_types.h"
#include <stdint.h>

// INTERNAL HELPERS

static void _lgui_strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void _lgui_itoa(uint32_t n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16]; int i = 0;
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void _lgui_strcat(char* dst, const char* src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

// Hitung panjang string
static int _lgui_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

// ------------------------------------------------------------
// Phase 4 — damage tracking.
// Setiap primitif yang menyentuh canvas mencatat rect yang MUNGKIN berubah.
// Union bbox ini di-upload gui_flush() via syscall 66, bukan seluruh canvas.
// Rect sedikit lebih besar BOLEH; lebih kecil (meninggalkan pixel basi) TIDAK.
// ------------------------------------------------------------
static void _lgui_damage(gui_window_t* win, int x, int y, int w, int h) {
    if (!win || w <= 0 || h <= 0) return;
    int W = (int)win->width, H = (int)win->height;
    // Clip ke [0,W) x [0,H). Guard overflow: x <= -w berarti seluruhnya di kiri.
    if (x < 0) { if (x <= -w) return; w += x; x = 0; }
    if (y < 0) { if (y <= -h) return; h += y; y = 0; }
    if (x >= W || y >= H) return;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;

    if (!win->dmg_valid) {
        win->dmg_valid = 1;
        win->dmg_x = x; win->dmg_y = y; win->dmg_w = w; win->dmg_h = h;
        return;
    }
    int x1 = win->dmg_x + win->dmg_w, y1 = win->dmg_y + win->dmg_h;
    if (x < win->dmg_x) win->dmg_x = x;
    if (y < win->dmg_y) win->dmg_y = y;
    if (x + w > x1) x1 = x + w;
    if (y + h > y1) y1 = y + h;
    win->dmg_w = x1 - win->dmg_x;
    win->dmg_h = y1 - win->dmg_y;
}

// Phase 5 — public damage API untuk penulis canvas langsung (libui: image/
// blend). Thin wrapper: clipping + union tetap di _lgui_damage (satu bbox).
void gui_damage_rect(gui_window_t* win, int x, int y, int w, int h) {
    _lgui_damage(win, x, y, w, h);
}

// CANVAS DRAWING — koordinat dalam canvas KONTEN (y=0 = baris isi
// pertama; titlebar milik WM tidak pernah digambar app).

// Canvas libgui selalu opaque (dulu `color | 0xFF000000`); API publik sekarang
// menerima `color_t` (include/libgui.h) dan diserialisasi lewat libs/gui/color di
// sini — satu-satunya tempat warna jadi pixel 32-bit.
static inline uint32_t _lgui_px(color_t c) {
    c.a = 255;
    return color_to_u32(c, FORMAT_ARGB);
}

static void _lgui_fill_rect(gui_window_t* win, int x, int y, int w, int h, color_t color) {
    _lgui_damage(win, x, y, w, h);   // Phase 4: rect yang mungkin berubah
    uint32_t solid = _lgui_px(color);
    int W = (int)win->width;
    int H = (int)win->height;
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            if (px >= 0 && px < W && py >= 0 && py < H)
                win->canvas[(py * W) + px] = solid;
        }
    }
}

static void _lgui_draw_char_abs(gui_window_t* win, char c, int x, int y, color_t color) {
    if (c < 0 || c > 127) return;
    _lgui_damage(win, x, y, 8, 16);   // Phase 4: sel glyph 8x16
    const unsigned char* bitmap = font8x16[(int)(unsigned char)c];
    uint32_t solid = _lgui_px(color);
    int W = (int)win->width;
    int H = (int)win->height;
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 8; col++) {
            if (bitmap[row] & (0x80 >> col)) {
                int px = x + col, py = y + row;
                if (px >= 0 && px < W && py >= 0 && py < H)
                    win->canvas[(py * W) + px] = solid;
            }
        }
    }
}

static void _lgui_draw_string_abs(gui_window_t* win, const char* str, int x, int y, color_t color) {
    int cx = x, cy = y;
    for (int i = 0; str[i]; i++) {
        if (str[i] == '\n') { cy += 16; cx = x; }
        else { _lgui_draw_char_abs(win, str[i], cx, cy, color); cx += 8; }
    }
}

// PUBLIC API

gui_window_t* gui_create_window(uint32_t width, uint32_t height) {
    gui_window_t* win = (gui_window_t*)sys_alloc(sizeof(gui_window_t));
    if (!win) return 0;

    win->canvas = (uint32_t*)sys_alloc(width * height * 4);
    if (!win->canvas) { sys_free(win); return 0; }

    win->width    = width;
    win->height   = height;
    win->inner_w  = width;
    win->inner_h  = height;
    win->is_running = 1;
    win->mouse_x  = 0;
    win->mouse_y  = 0;
    win->rel_x    = 0;
    win->rel_y    = 0;
    win->on_render = 0;
    win->dmg_valid = 0;   // Phase 4

    // Phase 5C: canvas = konten murni; width/height = ukuran konten.
    win->win_id = sys_kwm_create_window(100, 80, width, height);
    if (win->win_id < 0) {
        sys_free(win->canvas);
        sys_free(win);
        return 0;
    }

    // Gambar latar awal (konten penuh) + upload penuh awal.
    _lgui_fill_rect(win, 0, 0, (int)width, (int)height, COLOR_RGB(0xF5, 0xF5, 0xF5));
    sys_kwm_update_window(win->win_id, win->canvas);
    win->dmg_valid = 0;   // sudah ter-upload; jangan kirim ulang saat flush pertama

    // Phase 19: canvas barusan diisi penuh opaque (0xF5F5F5) dan semua primitif
    // libgui/libui memaksa alpha 0xFF, jadi scan seluruh canvas kernel menerima.
    // Ini mengaktifkan base-blit elision (Phase 16-18) untuk window ini. Gagal
    // ditolak = window tetap dirender lewat jalur scalar (aman).
    sys_kwm_set_window_opaque(win->win_id);

    return win;
}

// Phase 10: window DESKTOP — full-screen, frameless (z=0, no-focus).
gui_window_t* gui_create_desktop(void) {
    uint32_t sw = 0, sh = 0;
    if (sys_get_screen_size(&sw, &sh) != 0 || sw == 0 || sh == 0) return 0;

    gui_window_t* win = (gui_window_t*)sys_alloc(sizeof(gui_window_t));
    if (!win) return 0;

    win->canvas = (uint32_t*)sys_alloc(sw * sh * 4);
    if (!win->canvas) { sys_free(win); return 0; }

    win->width    = sw;
    win->height   = sh;
    win->inner_w  = sw;
    win->inner_h  = sh;
    win->is_running = 1;
    win->mouse_x  = 0;
    win->mouse_y  = 0;
    win->rel_x    = 0;
    win->rel_y    = 0;
    win->on_render = 0;
    win->dmg_valid = 0;   // Phase 4

    win->win_id = sys_kwm_create_desktop();
    if (win->win_id < 0) {
        sys_free(win->canvas);
        sys_free(win);
        return 0;
    }

    // Latar awal (wallpaper default) lalu update penuh.
    _lgui_fill_rect(win, 0, 0, (int)sw, (int)sh, COLOR_RGB(0x1E, 0x29, 0x3B));
    sys_kwm_update_window(win->win_id, win->canvas);
    win->dmg_valid = 0;   // sudah ter-upload

    // Phase 19: desktop = full-screen opaque wallpaper (lihat catatan di
    // gui_create_window). Semua pixel opaque → declare diterima; base blit untuk
    // tiap region yang tertutup desktop (seluruh layar) bisa dihilangkan.
    sys_kwm_set_window_opaque(win->win_id);

    return win;
}

// Phase 10: judul window (titlebar + taskbar).
int gui_set_window_title(gui_window_t* win, const char* title) {
    if (!win) return -1;
    return sys_kwm_set_title(win->win_id, title);
}

void gui_set_render(gui_window_t* win, gui_render_fn fn) {
    if (win) win->on_render = fn;
}

void gui_flush(gui_window_t* win) {
    if (!win || !win->dmg_valid) return;   // Phase 4: tanpa gambar baru → tanpa upload

    int x = win->dmg_x, y = win->dmg_y, w = win->dmg_w, h = win->dmg_h;

    // Damage seluruh window → syscall 31 (canvas penuh) lebih sederhana.
    if (x == 0 && y == 0 && w == (int)win->width && h == (int)win->height) {
        sys_kwm_update_window(win->win_id, win->canvas);
        win->dmg_valid = 0;
        return;
    }

    // Parsial: kirim HANYA rect; buffer tetap canvas penuh (stride = width*4).
    kwm_rect_update_t req;
    req.win_id = win->win_id;
    req.x = x; req.y = y;
    req.width  = (uint32_t)w;
    req.height = (uint32_t)h;
    req.buffer = win->canvas;
    if (sys_kwm_update_window_rect(&req) == 0)
        win->dmg_valid = 0;   // gagal → simpan damage agar dicoba lagi
}

void gui_destroy(gui_window_t* win) {
    if (!win) return;
    sys_kwm_destroy_window(win->win_id);
    sys_free(win->canvas);
    sys_free(win);
}

// EVENT LOOP UTAMA

void gui_mainloop(gui_window_t* win) {
    if (!win) return;
    kyuzen_event_t ev;

    while (win->is_running) {
        if (sys_get_event(&ev)) {

            // --- Pergerakan Mouse (koordinat window-local konten) ---
            if (ev.type == EVENT_MOUSE_MOVE) {
                win->mouse_x = ev.param1;
                win->mouse_y = ev.param2;
                win->rel_x = ev.param1;
                win->rel_y = ev.param2;
            }

            // --- Klik Kiri Ditekan ---
            if (ev.type == EVENT_MOUSE_CLICK && ev.param1 == 0 && ev.param2 == 1) {
                if (ev.param3 != 0) win->mouse_x = ev.param3;
                win->rel_x = win->mouse_x;
                win->rel_y = win->mouse_y;
            }

            // --- Phase 5C: WM minta app menutup (tombol close titlebar).
            // Bukan destroy paksa — app cleanup lalu sys_exit.
            if (ev.type == EVENT_WIN_CLOSE) {
                win->is_running = 0;
                break;
            }

            // --- Keyboard ESC ---
            if (ev.type == EVENT_KEY_PRESS && ev.param1 == 27) {
                win->is_running = 0;
                break;
            }
        }

        // Panggil render callback app (jika ada)
        if (win->on_render) {
            win->on_render(win);
            gui_flush(win);
        }

        sys_yield();
    }
}

// DRAWING API — koordinat RELATIF ke KONTEN window

void gui_draw_rect(gui_window_t* win, int x, int y, int w, int h, color_t color) {
    if (!win) return;
    _lgui_fill_rect(win, x, y, w, h, color);
}

void gui_draw_char(gui_window_t* win, char c, int x, int y, color_t color) {
    if (!win) return;
    _lgui_draw_char_abs(win, c, x, y, color);
}

void gui_draw_text(gui_window_t* win, const char* text, int x, int y, color_t color) {
    if (!win) return;
    _lgui_draw_string_abs(win, text, x, y, color);
}

void gui_draw_label_num(gui_window_t* win, const char* label, uint32_t num,
                        const char* suffix, int x, int y, color_t color) {
    char buf[128];
    _lgui_strncpy(buf, label, 80);
    char num_str[16];
    _lgui_itoa(num, num_str);
    _lgui_strcat(buf, num_str);
    if (suffix && suffix[0]) _lgui_strcat(buf, suffix);
    gui_draw_text(win, buf, x, y, color);
}

void gui_draw_bar(gui_window_t* win, int x, int y, int w, int h,
                  uint32_t value, uint32_t max_value, color_t bar_color) {
    if (!win || max_value == 0) return;
    // Background bar (abu-abu gelap)
    _lgui_fill_rect(win, x, y, w, h, COLOR_RGB(0x33, 0x33, 0x33));
    // Isi bar
    int filled = (int)((value * (uint32_t)w) / max_value);
    if (filled > w) filled = w;
    if (filled > 0)
        _lgui_fill_rect(win, x, y, filled, h, bar_color);
}
