#ifndef IO_H
#define IO_H

// =====================================================================
// SHIM include/io.h untuk TEST HOST — bukan untuk kernel.
//
// test/ata_devmodel_test.c meng-include drivers/ata.c apa adanya (pola sama
// dengan test/kyuzenfs_v4_test.c yang meng-include kernel/fs/bcache.c).
// Supaya driver ASLI itu bisa jalan di host, semua port I/O di sini
// diarahkan ke MODEL DEVICE ATA (test/ata_devmodel_test.c) lewat -iquote
// test/atamock yang didahulukan atas -iquote include (lihat Makefile).
//
// Catatan penting: `insw_rep` di sini adalah PADANAN FUNGSIONAL, bukan asm.
// Yang diuji test ini adalah LOGIKA driver (protokol, LBA, error, urutan
// baca). Instruksi asli `cld; rep insw` tidak bisa dieksekusi di host dan
// divalidasi lewat boot QEMU (lihat laporan Stage 1).
// =====================================================================

#include <stdint.h>

extern uint8_t  atamock_inb(uint16_t port);
extern void     atamock_outb(uint16_t port, uint8_t val);
extern uint16_t atamock_inw(uint16_t port);
extern void     atamock_outw(uint16_t port, uint16_t val);

static inline void     outb(uint16_t port, uint8_t val)  { atamock_outb(port, val); }
static inline uint8_t  inb (uint16_t port)               { return atamock_inb(port); }
static inline uint16_t inw (uint16_t port)               { return atamock_inw(port); }
static inline void     outw(uint16_t port, uint16_t val) { atamock_outw(port, val); }

// Padanan fungsional `rep insw`: satu word per panggilan, sama seperti
// helper asli yang membaca 256 word dari port data.
static inline void insw_rep(uint16_t port, void *buf, uint32_t count) {
    uint16_t *w = (uint16_t *)buf;
    for (uint32_t i = 0; i < count; i++) w[i] = atamock_inw(port);
}

#endif // IO_H
