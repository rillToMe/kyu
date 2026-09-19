#ifndef LIBGUI_H
#define LIBGUI_H

#include <stdint.h>
#include "userlib.h"
#include "color_types.h"   // API gambar memakai color_t (libs/color)

// ============================================================
// Kyuzen GUI Framework (libgui)
//
// Cara pakai:
//   gui_window_t* win = gui_create_window(400, 300);
//   gui_set_render(win, my_render_fn);
//   gui_mainloop(win);   ← blocks sampai window ditutup
//   gui_destroy(win);
//   sys_exit();
//
// Phase 5C: canvas = KONTEN MURNI. Titlebar + tombol close digambar WM
// (compositor), bukan oleh app. Koordinat mouse/klik yang diterima app
// sudah WINDOW-LOCAL terhadap konten (y=0 = baris isi pertama) — tidak
// perlu konversi layar. Tutup window via EVENT_WIN_CLOSE (WM) atau ESC.
// ============================================================

// --- Objek Window Utama ---
typedef struct gui_window_t gui_window_t;

typedef void (*gui_render_fn)(gui_window_t* win);

struct gui_window_t {
    int         win_id;
    uint32_t    width;      // Lebar canvas == lebar KONTEN
    uint32_t    height;     // Tinggi canvas == tinggi KONTEN
    uint32_t    inner_w;    // Lebar area isi (= width)
    uint32_t    inner_h;    // Tinggi area isi (= height)
    uint32_t*   canvas;     // Pointer ke pixel buffer
    int         is_running;

    // State mouse — koordinat window-local konten, di-update gui_mainloop
    int         mouse_x;
    int         mouse_y;
    int         rel_x;
    int         rel_y;

    // User render callback — dipanggil setiap ada update
    gui_render_fn on_render;

    // Phase 4: damage tracking — union bbox dari gambar sejak flush terakhir,
    // koordinat konten window-local. dmg_valid=0 → tidak ada yang berubah,
    // gui_flush() tidak meng-upload apa pun.
    int         dmg_valid;
    int         dmg_x, dmg_y, dmg_w, dmg_h;
};

// ============================================================
// API PUBLIC
// ============================================================

// Buat window baru. width / height = ukuran KONTEN; frame (titlebar milik
// WM) dihitung kernel. Dekorasi digambar compositor, bukan di canvas app.
gui_window_t* gui_create_window(uint32_t width, uint32_t height);

// Phase 10: buat window DESKTOP — full-screen, frameless (tanpa titlebar),
// z=0, no-focus. Canvas = ukuran layar penuh. Dipakai app shell desktop.
gui_window_t* gui_create_desktop(void);

// Phase 10: set judul window (ditampilkan di titlebar + taskbar). 0 / -1.
int gui_set_window_title(gui_window_t* win, const char* title);

// Set fungsi render — dipanggil tiap frame.
void gui_set_render(gui_window_t* win, gui_render_fn fn);

// Jalankan event loop. Blocks sampai user klik X (EVENT_WIN_CLOSE), klik X
// titlebar WM, atau tekan ESC. Memanggil on_render setiap kali ada event.
void gui_mainloop(gui_window_t* win);

// Hancurkan window dan bebaskan canvas.
void gui_destroy(gui_window_t* win);

// --- Drawing API ---
// Semua koordinat RELATIF terhadap KONTEN window. y=0 = baris isi pertama
// (di bawah titlebar milik WM).
//
// Warna memakai `color_t` (libs/color) — bukan hex 0xAARRGGBB lagi. Tulis
// komponennya dengan COLOR_RGB()/COLOR_RGBA() atau ambil dari palet
// (COLOR_WHITE, COLOR_BLACK, ...). Canvas libgui selalu opaque: byte alpha
// warna di sini dipaksa 255 saat ditulis, jadi warna transparan tetap
// menggambar (mask transparansi window dikelola compositor, bukan app).

// Isi persegi panjang dengan warna solid.
void gui_draw_rect(gui_window_t* win, int x, int y, int w, int h, color_t color);

// Gambar satu karakter (dari font 8x16 built-in).
void gui_draw_char(gui_window_t* win, char c, int x, int y, color_t color);

// Gambar string teks.
void gui_draw_text(gui_window_t* win, const char* text, int x, int y, color_t color);

// Gambar string dengan angka uint32_t di belakangnya (misal: "RAM: 128 MB").
void gui_draw_label_num(gui_window_t* win, const char* label, uint32_t num,
                        const char* suffix, int x, int y, color_t color);

// Gambar progress bar horizontal.
// value/max = rasio (0..max), bar_color = warna isi.
void gui_draw_bar(gui_window_t* win, int x, int y, int w, int h,
                  uint32_t value, uint32_t max_value, color_t bar_color);

// Paksa flush canvas ke layar (biasanya dipanggil otomatis oleh mainloop).
void gui_flush(gui_window_t* win);

// Phase 5: catat rect yang berubah ke damage bbox libgui. Untuk penulis
// canvas langsung (mis. libui image()/blend()) agar ikut partial-damage.
void gui_damage_rect(gui_window_t* win, int x, int y, int w, int h);

#endif // LIBGUI_H
