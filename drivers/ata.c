#include "ata.h"
#include "io.h"

#define ATA_DATA_PORT         0x1F0
#define ATA_SECTOR_COUNT_PORT 0x1F2
#define ATA_LBA_LO_PORT       0x1F3
#define ATA_LBA_MID_PORT      0x1F4
#define ATA_LBA_HI_PORT       0x1F5
#define ATA_DRIVE_PORT        0x1F6
#define ATA_COMMAND_PORT      0x1F7
#define ATA_STATUS_PORT       0x1F7
#define ATA_ALT_STATUS_PORT   0x3F6  // Alternate Status — baca TANPA clear interrupt

// === WAJIB OLEH SPESIFIKASI ATA: 400ns delay setelah drive select ===
// Setiap inb() pada bus ISA memakan ~100ns. 4 kali = ~400ns.
// Gunakan Alternate Status (0x3F6) agar tidak men-clear pending interrupt.
static void ata_delay_400ns(void) {
    inb(ATA_ALT_STATUS_PORT);
    inb(ATA_ALT_STATUS_PORT);
    inb(ATA_ALT_STATUS_PORT);
    inb(ATA_ALT_STATUS_PORT);
}

// Tunggu BSY bit clear, dengan proteksi timeout
static int ata_wait_bsy(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(ATA_STATUS_PORT);
        if (!(status & 0x80)) return 0; // BSY clear = OK
    }
    return -1; // Timeout
}

// Tunggu DRQ set + BSY clear, dengan proteksi ERR/DF dan timeout
static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(ATA_STATUS_PORT);
        if (status & 0x01) return -1;   // ERR bit = error!
        if (status & 0x20) return -2;   // DF bit = drive fault!
        if (!(status & 0x80) && (status & 0x08)) return 0; // !BSY && DRQ = siap!
    }
    return -3; // Timeout
}

void ata_read_sector(uint32_t lba, uint8_t* buffer) {
    // 1. Tunggu sampai disk tidak sibuk
    ata_wait_bsy();
    
    // 2. Pilih drive (Master = 0xE0) dan sisipkan 4 bit teratas dari LBA
    outb(ATA_DRIVE_PORT, 0xE0 | ((lba >> 24) & 0x0F));
    ata_delay_400ns();  // KRITIS: Tunggu drive select settle (ATA spec)
    
    // 3. Tentukan jumlah sektor yang mau dibaca (1 sektor)
    outb(ATA_SECTOR_COUNT_PORT, 1);
    
    // 4. Kirim sisa 24 bit alamat LBA
    outb(ATA_LBA_LO_PORT, (uint8_t)lba);
    outb(ATA_LBA_MID_PORT, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI_PORT, (uint8_t)(lba >> 16));
    
    // 5. Kirim perintah READ SECTORS (0x20)
    outb(ATA_COMMAND_PORT, 0x20);
    
    // 6. Tunggu 400ns sebelum mulai polling (ATA spec)
    ata_delay_400ns();
    
    // 7. Polling: Tunggu DRQ dengan pengecekan error
    if (ata_wait_drq() != 0) {
        // Error! Isi buffer dengan nol agar tidak crash
        for (int i = 0; i < 512; i++) buffer[i] = 0;
        return;
    }
    
    // 8. Sedot Datanya! 256 kali loop x 2 byte = 512 byte (1 sektor)
    uint16_t* ptr = (uint16_t*)buffer;
    for (int i = 0; i < 256; i++) {
        ptr[i] = inw(ATA_DATA_PORT);
    }
}

void ata_write_sector(uint32_t lba, uint8_t* buffer) {
    // 1. Tunggu sampai disk tidak sibuk
    ata_wait_bsy();
    
    // 2. Pilih drive dan alamat LBA
    outb(ATA_DRIVE_PORT, 0xE0 | ((lba >> 24) & 0x0F));
    ata_delay_400ns();  // KRITIS: Tunggu drive select settle (ATA spec)
    
    // 3. Setel parameter sektor
    outb(ATA_SECTOR_COUNT_PORT, 1);
    outb(ATA_LBA_LO_PORT, (uint8_t)lba);
    outb(ATA_LBA_MID_PORT, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI_PORT, (uint8_t)(lba >> 16));
    
    // 4. Kirim perintah WRITE SECTORS (0x30)
    outb(ATA_COMMAND_PORT, 0x30);
    
    // 5. Tunggu 400ns sebelum mulai polling (ATA spec)
    ata_delay_400ns();
    
    // 6. Tunggu DRQ dengan pengecekan error
    if (ata_wait_drq() != 0) return;  // Abort jika error
    
    // 7. Suntikkan Data! 256 kali loop x 2 byte = 512 byte (1 sektor)
    uint16_t* ptr = (uint16_t*)buffer;
    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA_PORT, ptr[i]);
    }
    
    // 8. SANGAT PENTING: Perintah CACHE FLUSH (0xE7)
    outb(ATA_COMMAND_PORT, 0xE7);
    ata_delay_400ns();
    ata_wait_bsy();  // Tunggu flush selesai
}

// Fungsi untuk menanyakan ukuran asli Hard Disk ke Hardware QEMU via ATA IDENTIFY
//
// CATATAN PROTOKOL (ini pernah salah dan bikin crashdump/format dianggap
// "disk terlalu kecil"): setelah memilih drive WAJIB tunggu 400ns sebelum
// mengirim IDENTIFY, dan setelah perintah WAJIB 400ns lagi sebelum membaca
// status. Tanpa itu status pertama terbaca 0x00 (drive belum menjawab) dan
// fungsi ini salah melaporkan "tidak ada drive" padahal baca/tulis sektor
// normal jalan. Polling juga memakai helper yang memeriksa ERR/DF, bukan
// while() tanpa batas (bisa menggantung di hardware nyata).
#define ATA_IDENTIFY_WORDS 256u
#define ATA_WORD_SUPPORTED_LBA48 83u
#define ATA_WORD_LBA28_LO        60u
#define ATA_WORD_LBA48_LO        100u

uint32_t ata_get_total_sectors(void) {
    ata_wait_bsy();                       // drive harus tidak sibuk dulu
    outb(ATA_DRIVE_PORT, 0xE0);           // Master, LBA mode
    ata_delay_400ns();                    // drive select settle
    outb(ATA_COMMAND_PORT, 0xEC);         // IDENTIFY DEVICE
    ata_delay_400ns();

    uint8_t status = inb(ATA_STATUS_PORT);
    if (status == 0x00 || status == 0xFF) return 0;   // tidak ada drive
    if (ata_wait_drq() != 0) return 0;                // ERR/DF/timeout

    uint16_t buffer[ATA_IDENTIFY_WORDS];
    for (uint32_t i = 0; i < ATA_IDENTIFY_WORDS; i++) buffer[i] = inw(ATA_DATA_PORT);

    // LBA48 (kalau didukung dan dilaporkan) lebih tepat daripada LBA28.
    if ((buffer[ATA_WORD_SUPPORTED_LBA48] & 0x0400u) &&
        (buffer[ATA_WORD_LBA48_LO + 2u] | buffer[ATA_WORD_LBA48_LO + 3u])) {
        uint64_t lba48 = (uint64_t)buffer[ATA_WORD_LBA48_LO] |
                         ((uint64_t)buffer[ATA_WORD_LBA48_LO + 1u] << 16) |
                         ((uint64_t)buffer[ATA_WORD_LBA48_LO + 2u] << 32) |
                         ((uint64_t)buffer[ATA_WORD_LBA48_LO + 3u] << 48);
        // API ini 32-bit (LBA28); clamp supaya pemanggil tidak overflow.
        return (lba48 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)lba48;
    }

    return (uint32_t)buffer[ATA_WORD_LBA28_LO] |
           ((uint32_t)buffer[ATA_WORD_LBA28_LO + 1u] << 16);
}

// =====================================================================
// Wrapper Block 4KB — Modul 1 KyuzenFS V4.
// 1 block = 8 sektor LBA kontigu. Dibaca/ditulis sebagai 8 transfer
// sektor tunggal (driver dasar memang PIO 1-sektor); bcache yang
// menyatukan semuanya sebagai satu unit cache.
// =====================================================================
#define KZFS_BLOCK_SECTORS 8

int ata_read_block4k(uint64_t block_num, void *buf) {
    if (!buf) return -1;
    uint8_t *p = (uint8_t*)buf;
    uint32_t lba0 = (uint32_t)(block_num * KZFS_BLOCK_SECTORS);
    for (int s = 0; s < KZFS_BLOCK_SECTORS; s++) {
        ata_read_sector(lba0 + (uint32_t)s, p + (s * 512));
    }
    return 0;
}

int ata_write_block4k(uint64_t block_num, const void *buf) {
    if (!buf) return -1;
    const uint8_t *p = (const uint8_t*)buf;
    uint32_t lba0 = (uint32_t)(block_num * KZFS_BLOCK_SECTORS);
    for (int s = 0; s < KZFS_BLOCK_SECTORS; s++) {
        ata_write_sector(lba0 + (uint32_t)s, (uint8_t*)(p + (s * 512)));
    }
    return 0;
}