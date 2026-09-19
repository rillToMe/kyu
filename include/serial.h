#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>   // serial_dec() memakai uint64_t

void serial_init(void);
void serial_putc(char c);
void serial_print(const char* s);
void serial_print_hex(uint64_t v);   // cetak hex 64-bit "0x..." lebar-penuh
void serial_dec(uint64_t v);   // cetak bilangan desimal tak bertanda

// Batasi total waktu menunggu UART siap (dipakai jalur panic).
// KENAPA: serial_putc() normal adalah `while (!(inb(LSR) & THRE));` — TAK
// BERBATAS. Kalau fault terjadi saat backend serial macet/tidak mengalir,
// handler panic akan berputar di situ SELAMANYA: layar BSOD tidak pernah
// muncul dan sistem tampak "freeze tanpa panic". Dipanggil paling awal di
// kedua entry panic; setelah budget habis sisa dump dibuang (byte drop jauh
// lebih baik daripada kehilangan seluruh laporan).
void serial_enter_panic_mode(void);
#endif
