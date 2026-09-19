// kernel/kprint.c — Konsol kernel (TTY + serial mirror), terpisah dari FS.
//
// Sebelumnya kprint tinggal di kernel/kyuzenfs.c (V3). Dengan KyuzenFS V4,
// konsol jadi modul sendiri supaya FS tidak lagi menjadi "pemilik" output
// kernel — driver FS boleh diganti tanpa menyentuh jalur logging.
//
// Boot-console separation: saat kprint_quiet = 1, output ditahan dari
// TTY/framebuffer dan dialirkan ke COM1 (serial) saja. Dipakai kernel_main
// untuk meredam log verbose subsistem selama boot. 0 = perilaku normal.

#include "fs.h"
#include <stdint.h>

extern fs_node_t tty_node;
extern void serial_print(const char* s);
extern int  g_serial_ready;

int kprint_quiet = 0;

static uint32_t ksl(const char* s) {
    uint32_t l = 0; while (s[l]) l++; return l;
}

void kprint(const char* str) {
    if (!str) return;

    // Mode verbose: diagnostik penuh → serial, console tetap bersih.
    if (kprint_quiet) {
        if (g_serial_ready) serial_print(str);
        return;
    }

    if (!tty_node.write) return;

#ifdef HEAP_WATCH_DEBUG
    // Mirror kprint to COM1 so boot log survives a watchpoint freeze / BSOD,
    // where the framebuffer TTY is no longer readable on the host side.
    serial_print(str);
#endif

    // NOTE: No lock here — TTY output may interleave across CPUs but the
    // callers never hold a lock that tty_write could invert.
    write_fs(&tty_node, 0, ksl(str), (uint8_t*)str);
    extern void compositor_flush(void);
    compositor_flush();
}

// Wrapper kprint_num(uint64_t) — dideklarasikan eksplisit di caller lama.
void kprint_num(uint64_t num) {
    if (num == 0) { kprint("0"); return; }
    char buf[21];
    int i = 19;
    buf[20] = '\0';
    while (num > 0) { buf[i--] = (char)((num % 10) + '0'); num /= 10; }
    kprint(&buf[i + 1]);
}
