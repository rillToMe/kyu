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
#endif