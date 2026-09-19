#ifndef ATA_H
#define ATA_H

#include <stdint.h>

// Fungsi untuk membaca 1 sektor (512 byte) dari Hard Disk
void ata_read_sector(uint32_t lba, uint8_t* buffer);
void ata_write_sector(uint32_t lba, uint8_t* buffer);
uint32_t ata_get_total_sectors(void);

// Wrapper Block 4KB (Modul 1 KyuzenFS V4): 1 block = 8 sektor LBA.
// Dipakai bcache; return 0 sukses, -1 argumen tidak valid.
int ata_read_block4k(uint64_t block_num, void *buf);
int ata_write_block4k(uint64_t block_num, const void *buf);

// =====================================================================
// Jalur baca, Stage 1
// Desain: DOCUMENTATION/design/ata-driver-redesign-proposal.md
//
// DUA jalur SELALU dikompilasi; flag hanya memilih default saat boot:
//   ATA_READ_PATH_LEGACY (0) → loop 8x ata_read_sector, perilaku lama persis
//   ATA_READ_PATH_BATCH  (1) → satu perintah READ SECTORS count>1, transfer
//                              data lewat `rep insw` (satu instruksi/sektor)
//
// Default 0 (jalur lama) sampai jalur baru tervalidasi. Override saat build:
//   make ATA_READ_PATH_DEFAULT=1
// Jalur lama TIDAK PERNAH dihapus: dia fallback permanen untuk crashdump
// (polling tanpa IRQ), device tanpa multi-sector, dan rollback cepat.
// =====================================================================
#define ATA_READ_PATH_LEGACY 0
#define ATA_READ_PATH_BATCH  1

#ifndef ATA_READ_PATH_DEFAULT
#define ATA_READ_PATH_DEFAULT ATA_READ_PATH_LEGACY
#endif

// Sektor maksimum per satu perintah multi-sektor (1 blok bcache = 8 sektor).
#define ATA_READ_BATCH_MAX_SECTORS 8u

// Jalur baca aktif saat ini (di-inisialisasi dari ATA_READ_PATH_DEFAULT).
int  ata_read_path_get(void);
void ata_read_path_set(int path);

// Primitif rentang (Stage 1): baca `count` sektor mulai `lba` ke `buf`.
// Berlaku untuk kedua jalur (LEGACY: loop per sektor; BATCH: satu perintah
// untuk ≤ ATA_READ_BATCH_MAX_SECTORS sektor + `rep insw`).
// Return: 0 sukses; -1 argumen invalid; -2 device error (ERR/DF);
//         -3 timeout; -4 di luar jangkauan LBA28 (tanpa menyentuh hardware).
int  ata_read_range(uint32_t lba, uint32_t count, uint8_t *buf);

#endif