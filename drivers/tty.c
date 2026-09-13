#include "tty.h"
#include <stddef.h>
#include <stdint.h>

// 1. Impor variabel dan fungsi GUI dari kernel.c
extern uint32_t* fb_ptr;
extern uint32_t fb_width;
extern uint32_t fb_height;
extern uint32_t fb_pitch;

extern void draw_rect(uint32_t start_x, uint32_t start_y, uint32_t width, uint32_t height, uint32_t color);
extern void draw_char(char c, uint32_t x, uint32_t y, uint32_t color);

// Compositor (Phase 3B) hanya menampilkan area yang ditandai dirty. tty_scroll
// menggeser pixel base_canvas langsung, jadi area geser itu wajib ditandai.
extern void screen_mark_dirty(int32_t x, int32_t y, uint32_t width, uint32_t height);

// Serial COM1: mirror output TTY. Kernel serial sudah selalu di-init
// (kernel.c — panic dump & watchdog memakainya), jadi cukup dipanggil.
// Tujuan: output app TTY (mis. hello-rs via sys_print/syscall 1) tetap
// terbaca di host (QEMU -serial stdio / file:serial.log) walaupun TTY
// di layar tertutup jendela desktop/compositor.
extern void serial_putc(char c);

// tty_scroll harus scroll base_canvas (bukan backbuffer yang di-overwrite compositor tiap frame!)
extern uint32_t base_canvas[1920 * 1080];

// 2. Konstanta Terminal GUI
#define FONT_WIDTH 8
#define FONT_HEIGHT 16 // Tinggi diset 16 agar ada jarak kosong 8px di bawah setiap huruf
#define BG_COLOR 0x1E1E1E
#define FG_COLOR 0xFFFFFF

// Line Buffer (Phase 3D): riwayat teks terminal disimpan baris demi baris di
// ring buffer ini — bukan sebagai pixel. Output langsung tetap digambar
// incremental (jalur cepat lama, tanpa regresi); ring hanya sumber kebenaran
// untuk scrollback. Ukuran cukup untuk resolusi maksimum 1920x1080 (240 kolom).
#define TTY_HISTORY_LINES 512
#define TTY_MAX_COLS      256

size_t terminal_row;
size_t terminal_column;
fs_node_t tty_node;

static char     tty_history[TTY_HISTORY_LINES][TTY_MAX_COLS];
static uint32_t tty_line_count;   // indeks logis baris yang sedang ditulis (paling bawah)
static int32_t  tty_view_offset;  // 0 = tampilkan output terbaru; >0 = scroll ke riwayat

// --- ANIMASI KURSOR ---
int cursor_state = 1; // 1 = Menyala, 0 = Mati

static uint32_t tty_cols(void) { return fb_width / FONT_WIDTH; }
static uint32_t tty_rows(void) { return fb_height / FONT_HEIGHT; }

static char* tty_hist_line(uint32_t logical) {
    return tty_history[logical % TTY_HISTORY_LINES];
}

static void tty_hist_clear_line(uint32_t logical) {
    tty_hist_line(logical)[0] = '\0';
}

void tty_draw_cursor() {
    cursor_state = 1;
    if (tty_view_offset != 0) return; // Jangan gambar kursor di atas riwayat yang di-scroll
    draw_rect(terminal_column * FONT_WIDTH, (terminal_row * FONT_HEIGHT) + 14, FONT_WIDTH, 2, FG_COLOR);
}

void tty_blink_cursor() {
    if (tty_view_offset != 0) return;
    cursor_state = !cursor_state;
    if (cursor_state) draw_rect(terminal_column * FONT_WIDTH, (terminal_row * FONT_HEIGHT) + 14, FONT_WIDTH, 2, FG_COLOR);
    else draw_rect(terminal_column * FONT_WIDTH, (terminal_row * FONT_HEIGHT) + 14, FONT_WIDTH, 2, BG_COLOR);
}

void tty_erase_cursor() {
    if (tty_view_offset != 0) return;
    draw_rect(terminal_column * FONT_WIDTH, (terminal_row * FONT_HEIGHT) + 14, FONT_WIDTH, 2, BG_COLOR);
}

// --- FITUR SCROLLING LAYAR GUI ---
void tty_scroll() {
    uint32_t copy_height = fb_height - FONT_HEIGHT;
    uint32_t stride = fb_pitch / 4; // pixels per row
    for (uint32_t y = 0; y < copy_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            // Scroll base_canvas, bukan backbuffer!
            // compositor_flush() copy base_canvas -> backbuffer setiap frame,
            // jadi scroll di backbuffer langsung hilang.
            base_canvas[(y * stride) + x] = base_canvas[((y + FONT_HEIGHT) * stride) + x];
        }
    }
    draw_rect(0, fb_height - FONT_HEIGHT, fb_width, FONT_HEIGHT, BG_COLOR);
    screen_mark_dirty(0, 0, fb_width, fb_height);
    terminal_row--;
}

// Repaint seluruh layar dari ring sesuai view offset. Baris logis teratas yang
// terlihat = line_count - terminal_row - view_offset.
static void tty_render_view(void) {
    uint32_t rows = tty_rows();
    int32_t first = (int32_t)tty_line_count - (int32_t)terminal_row - tty_view_offset;

    for (uint32_t sr = 0; sr < rows; sr++) {
        draw_rect(0, sr * FONT_HEIGHT, fb_width, FONT_HEIGHT, BG_COLOR);
        int32_t logical = first + (int32_t)sr;
        if (logical < 0 || (uint32_t)logical > tty_line_count) continue;
        if (tty_line_count >= TTY_HISTORY_LINES &&
            (uint32_t)logical <= tty_line_count - TTY_HISTORY_LINES) continue; // sudah tergilas ring

        const char* text = tty_hist_line((uint32_t)logical);
        for (uint32_t col = 0; text[col] != '\0' && col < TTY_MAX_COLS; col++) {
            draw_char(text[col], col * FONT_WIDTH, sr * FONT_HEIGHT, FG_COLOR);
        }
    }
}

// Jumlah baris riwayat yang bisa di-scroll ke atas dari posisi live.
static int32_t tty_max_scrollback(void) {
    int32_t top_live = (int32_t)tty_line_count - (int32_t)terminal_row;
    int32_t oldest = 0;
    if (tty_line_count >= TTY_HISTORY_LINES)
        oldest = (int32_t)tty_line_count - (int32_t)TTY_HISTORY_LINES + 1;
    int32_t max = top_live - oldest;
    return max < 0 ? 0 : max;
}

void tty_scroll_view(int32_t delta_lines) {
    int32_t next = tty_view_offset + delta_lines;
    int32_t max = tty_max_scrollback();
    if (next < 0) next = 0;
    if (next > max) next = max;
    if (next == tty_view_offset) return;
    tty_view_offset = next;
    tty_render_view();
    if (tty_view_offset == 0) tty_draw_cursor();
}

static void tty_newline(void) {
    terminal_column = 0;
    tty_line_count++;
    tty_hist_clear_line(tty_line_count);
    if (++terminal_row >= tty_rows()) tty_scroll();
}

void terminal_putchar(char c) {
    // Output baru selalu melompat kembali ke bawah (perilaku terminal normal).
    if (tty_view_offset != 0) {
        tty_view_offset = 0;
        tty_render_view();
    }

    tty_erase_cursor();

    if (c == '\n') {
        tty_newline();
    }
    else if (c == '\b') {
        if (terminal_column > 0) {
            terminal_column--;
            tty_hist_line(tty_line_count)[terminal_column] = '\0';
        } else if (terminal_row > 0) {
            // ponytail: backspace di kolom 0 hanya mundur visual, tidak lintas
            // baris logis di ring — kasus langka, tidak merusak scrollback.
            terminal_row--;
            terminal_column = tty_cols() - 1;
        }
        draw_rect(terminal_column * FONT_WIDTH, terminal_row * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT, BG_COLOR);
    }
    else {
        draw_rect(terminal_column * FONT_WIDTH, terminal_row * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT, BG_COLOR);
        draw_char(c, terminal_column * FONT_WIDTH, terminal_row * FONT_HEIGHT, FG_COLOR);

        if (terminal_column < TTY_MAX_COLS - 1) {
            char* line = tty_hist_line(tty_line_count);
            line[terminal_column] = c;
            line[terminal_column + 1] = '\0';
        }

        if (++terminal_column >= tty_cols()) {
            tty_newline();
        }
    }

    tty_draw_cursor();
}

void tty_clear(void) {
    draw_rect(0, 0, fb_width, fb_height, BG_COLOR);
    terminal_row = 0;
    terminal_column = 0;
    tty_line_count = 0;
    tty_view_offset = 0;
    tty_hist_clear_line(0);
    tty_draw_cursor();
}

uint32_t tty_write(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    for (uint32_t i = 0; i < size; i++) {
        terminal_putchar(buffer[i]);
        // Mirror ke COM1 — terminasi \n jadi \r\n (serial terminal butuh CR).
        if (buffer[i] == '\n') serial_putc('\r');
        serial_putc((char)buffer[i]);
    }
    return size;
}

extern uint32_t keyboard_read(uint8_t *buffer, uint32_t size);
uint32_t tty_read(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    return keyboard_read(buffer, size);
}

fs_node_t* init_tty(void) {
    tty_clear();
    tty_node.name[0] = 't'; tty_node.name[1] = 't'; tty_node.name[2] = 'y'; tty_node.name[3] = '0'; tty_node.name[4] = '\0';
    tty_node.flags = FS_CHARDEVICE;
    tty_node.write = tty_write;
    tty_node.read = tty_read;
    return &tty_node;
}
