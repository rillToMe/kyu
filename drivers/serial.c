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

void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20));
    outb(COM1, (uint8_t)c);
}

void serial_print(const char* s) {
    while (*s) serial_putc(*s++);
}
