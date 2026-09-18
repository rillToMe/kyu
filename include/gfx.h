#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "display.h"

// --- Framebuffer global (didefinisikan di kernel/gfx/fb.c) ---
extern uint32_t* fb_ptr;

// Screen buffer mode-sized (dialokasikan display_alloc_buffers(); NULL sebelum
// itu). Stride kedua buffer == pitch_bytes/4 (kontrak lama, mempertahankan
// indeks compositor/TTY). Bukan lagi array statis 1920x1080.
extern uint32_t* backbuffer;
extern uint32_t* base_canvas;

// --- DisplayBuffer layar (Phase 3A adopsi, kernel/gfx/fb.c) ---
// Wrapper statis di atas base_canvas/backbuffer/fb_ptr. NULL sebelum
// framebuffer ditangkap kernel_main.
DisplayBuffer* gfx_screen_buffer(void);
DisplayBuffer* gfx_back_buffer(void);
DisplayBuffer* gfx_fb_buffer(void);

// --- Primitif gambar (kernel/gfx/fb.c) ---
// Kontrak Phase 3B: semua primitive menandai dirty sendiri — jangan
// tambahkan screen_mark_dirty manual setelah memanggilnya.
void draw_pixel(uint32_t x, uint32_t y, uint32_t color);
void draw_rect(uint32_t start_x, uint32_t start_y, uint32_t width, uint32_t height, uint32_t color);
void draw_image(int start_x, int start_y, int width, int height, uint32_t* buffer);
void draw_char(char c, uint32_t x, uint32_t y, uint32_t color);
void draw_string(const char* str, uint32_t x, uint32_t y, uint32_t color);

// --- Compositor dirty-region (kernel/gfx/compositor.c) ---
void screen_mark_dirty(int32_t x, int32_t y, uint32_t width, uint32_t height);
void compositor_flush(void);

// Phase 9 — bentuk kursor global (0 panah / 1 I-beam / 2 tangan).
// Dipanggil dari syscall 58; validator range di sini (syscall.c juga).
void kwm_set_cursor(int kind);

#endif
