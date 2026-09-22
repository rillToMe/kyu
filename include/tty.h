#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include "fs.h"

// Fungsi untuk menginisialisasi layar dan mengembalikan objek VFS-nya
fs_node_t* init_tty(void);
void tty_clear(void);

// Node TTY global (didefinisikan di drivers/tty.c) — kanal console untuk
// sys_print/sys_read_keyboard dan kprint. Deklarasi kanonis di sini agar
// konsumen tidak mengulang `extern` manual.
extern fs_node_t tty_node;

// Warna teks TTY berikutnya (boot status). Hanya memengaruhi glyph yang
// digambar setelahnya; tidak menyentuh compositor/KWM. Default FG_COLOR.
void tty_set_fg(uint32_t color);

// Scrollback: geser jendela tampilan `delta` baris (positif = ke riwayat lama,
// negatif = kembali ke output terbaru). Dipanggil Phase 4 dari mouse wheel.
void tty_scroll_view(int32_t delta_lines);

#endif