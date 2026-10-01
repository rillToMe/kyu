#ifndef KPRINT_H
#define KPRINT_H

#include <stdint.h>

// ============================================================
// Konsol kernel (kernel/kprint.c).
//
// Sebelumnya setiap TU menulis `extern void kprint(...)` sendiri. Deklarasi
// kanonis dipindah ke sini supaya signature-nya satu sumber dan tidak drift.
//
// `kprint_quiet` = 1 menahan output dari TTY/framebuffer dan mengalirkannya ke
// COM1 saja — dipakai boot untuk meredam log verbose subsistem.
// ============================================================

void kprint(const char* str);
void kprint_num(uint64_t num);

// Cetak angka 32-bit sebagai desimal (bukan hex, meski namanya "hex" —
// nama dipertahankan karena sudah dipakai lama oleh drivers/pci.c dan
// graphics/backend/intel/intel_init.c).
void print_hex(uint32_t num);

extern int kprint_quiet;

#endif // KPRINT_H
