#ifndef IO_H
#define IO_H
#include <stdint.h>

// Menulis 1 byte data ke port hardware
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

// Membaca 1 byte data dari port hardware
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

// Membaca 2 byte (16-bit) data dari port hardware
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ( "inw %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ( "outw %0, %1" : : "a"(val), "Nd"(port) );
}

// Baca `count` word (16-bit) dari port ke buffer memori memakai SATU instruksi
// string I/O (`rep insw`). Dipakai jalur baca ATA batch di drivers/ata.c.
//
// WAJIB `cld` di depan: arah string I/O ditentukan flag DF — kalau DF=1, data
// ditulis MUNDUR dari buffer (korupsi memori senyap). ABI x86_64 mensyaratkan
// DF=0, tapi instruksi ini tidak boleh mengasumsikannya (mis. dipanggil dari
// konteks ISR yang belum menormalkan flag).
static inline void insw_rep(uint16_t port, void* buf, uint32_t count) {
    __asm__ volatile ( "cld; rep insw"
                       : "+D"(buf), "+c"(count)
                       : "d"(port)
                       : "memory" );
}

// INI YANG BARU UNTUK PCI (32-Bit / Long)
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ( "inl %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

#endif