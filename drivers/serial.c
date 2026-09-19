#include "serial.h"
#include <stdint.h>

#define COM1 0x3F8

// Set 1 oleh serial_init(). Dipakai kprint_quiet (kernel/kyuzenfs.c) supaya
// mirror diagnostik ke COM1 hanya setelah UART benar-benar siap (menghindari
// spin di serial_putc menunggu LSR pada UART yang belum diinisialisasi).
int g_serial_ready = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
    g_serial_ready = 1;   // aman dipakai kprint_quiet sebagai mirror diagnostik
}

// === Mode panic: tunggu THRE dengan BUDGET, bukan selamanya ===
// Iterasi inb() ke port COM adalah I/O port (~0.3-1 us di VM), jadi 200000
// iterasi ≈ 0.1-0.2 s total untuk SELURUH dump — cukup untuk menyerap FIFO
// 16 byte yang mengalir normal, tapi tidak cukup untuk menggantungkan handler
// panic kalau backend serial mati. Setelah budget habis, byte dibuang.
#define SERIAL_PANIC_SPIN_BUDGET 200000u
static volatile uint32_t g_serial_spin_budget = 0;   // >0 = mode panic aktif

void serial_enter_panic_mode(void) {
    g_serial_spin_budget = SERIAL_PANIC_SPIN_BUDGET;
}

void serial_putc(char c) {
    if (g_serial_spin_budget) {
        while (!(inb(COM1 + 5) & 0x20)) {
            if (--g_serial_spin_budget == 0) return;   // UART macet: buang byte
        }
        outb(COM1, (uint8_t)c);
        return;
    }
    while (!(inb(COM1 + 5) & 0x20));
    outb(COM1, (uint8_t)c);
}

void serial_print(const char* s) {
    while (*s) serial_putc(*s++);
}

// Cetak bilangan desimal tak bertanda (lintas modul: kernel.c punya versi
// static-nya sendiri yang lebih tua; yang ini dipakai modul baru seperti
// kernel/crash_archive.c).
void serial_dec(uint64_t v) {
    char buf[21];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v && n < 20) { buf[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n) serial_putc(buf[--n]);
}
