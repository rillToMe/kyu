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
// 1 block = 8 sektor LBA kontigu. Kedua jalur baca ditulis sebagai fungsi
// terpisah; ata_read_block4k memilih lewat flag (lihat include/ata.h).
// =====================================================================
#define KZFS_BLOCK_SECTORS 8

// =====================================================================
// JALUR BACA STAGE 1                        (lihat DOCUMENTATION/design/
//                                            ata-driver-redesign-proposal.md)
//
//   LEGACY : 8x ata_read_sector — satu perintah per sektor. Perilaku PERSIS
//            seperti sebelum Stage 1 (termasuk silent-zero saat error).
//   BATCH  : satu perintah READ SECTORS untuk <= 8 sektor + transfer data
//            lewat `rep insw` (satu instruksi per 512 B, bukan 256 inw).
//
// Keduanya selalu dikompilasi; g_read_path hanya memilih mana yang dipakai.
// Static di bawah ini bertipe trivial — tidak ada constructor global
// (syarat ELF loader kernel: .init_array tidak dijalankan).
// =====================================================================
static int g_read_path =
    (ATA_READ_PATH_DEFAULT == ATA_READ_PATH_BATCH) ? ATA_READ_PATH_BATCH
                                                   : ATA_READ_PATH_LEGACY;

int  ata_read_path_get(void) { return g_read_path; }
void ata_read_path_set(int path) {
    g_read_path = (path == ATA_READ_PATH_BATCH) ? ATA_READ_PATH_BATCH
                                                : ATA_READ_PATH_LEGACY;
}

// Jalur BATCH: satu perintah untuk `count` (1..8) sektor kontigu.
// Return 0 sukses; -1 argumen; -2 device error; -3 timeout; -4 LBA28 overflow.
static int ata_read_sectors_batched(uint32_t lba, uint32_t count, uint8_t *buf) {
    if (!buf || count == 0 || count > ATA_READ_BATCH_MAX_SECTORS) return -1;
    // Jangan pernah mengirim perintah yang membungkus LBA28 ke LBA kecil:
    // pembungkusan = baca sektor yang SALAH tanpa error. Ditolak sebelum
    // menyentuh hardware.
    if (lba > 0x0FFFFFFFu || (lba + count - 1u) > 0x0FFFFFFFu) return -4;

    // Beda dari jalur lama (yang mengabaikan hasil wait_bsy): di sini timeout
    // BSY = berhenti, bukan lanjut mengirim perintah ke drive yang masih sibuk.
    if (ata_wait_bsy() != 0) return -3;

    outb(ATA_DRIVE_PORT, 0xE0 | ((lba >> 24) & 0x0F));
    ata_delay_400ns();

    outb(ATA_SECTOR_COUNT_PORT, (uint8_t)count);   // >1 = perintah multi-sektor
    outb(ATA_LBA_LO_PORT, (uint8_t)lba);
    outb(ATA_LBA_MID_PORT, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI_PORT, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND_PORT, 0x20);                  // READ SECTORS (0x20)
    ata_delay_400ns();

    // DRQ di-assert per sektor (512 B). Transfer satu sektor = satu `rep insw`.
    // Polling status di antara sektor WAJIB: membaca data port saat DRQ mati
    // = data sampah / sektor salah (device tidak melanjutkan ke sektor
    // berikutnya sebelum yang sekarang selesai dibaca).
    for (uint32_t s = 0; s < count; s++) {
        int rc = ata_wait_drq();
        if (rc != 0) return (rc == -3) ? -3 : -2;   // ERR/DF -> -2, timeout -> -3
        insw_rep(ATA_DATA_PORT, buf + (s * 512u), 256u);
    }
    return 0;
}

// Primitif rentang — dipakai kedua jalur (dan nanti oleh read-ahead bcache).
int ata_read_range(uint32_t lba, uint32_t count, uint8_t *buf) {
    if (!buf || count == 0) return -1;

    if (g_read_path == ATA_READ_PATH_LEGACY) {
        for (uint32_t s = 0; s < count; s++) {
            ata_read_sector(lba + s, buf + (s * 512u));   // error = buffer nol
        }
        return 0;
    }

    while (count > 0) {
        uint32_t n = (count > ATA_READ_BATCH_MAX_SECTORS)
                     ? ATA_READ_BATCH_MAX_SECTORS : count;
        int rc = ata_read_sectors_batched(lba, n, buf);
        if (rc != 0) return rc;
        lba   += n;
        buf   += n * 512u;
        count -= n;
    }
    return 0;
}

// Jalur LEGACY (logika lama, dipindah apa adanya): 8 perintah sektor tunggal.
static int ata_read_block4k_legacy(uint64_t block_num, void *buf) {
    if (!buf) return -1;
    uint8_t *p = (uint8_t*)buf;
    uint32_t lba0 = (uint32_t)(block_num * KZFS_BLOCK_SECTORS);
    for (int s = 0; s < KZFS_BLOCK_SECTORS; s++) {
        ata_read_sector(lba0 + (uint32_t)s, p + (s * 512));
    }
    return 0;
}

int ata_read_block4k(uint64_t block_num, void *buf) {
    if (!buf) return -1;
    if (g_read_path == ATA_READ_PATH_BATCH) {
        uint32_t lba0 = (uint32_t)(block_num * KZFS_BLOCK_SECTORS);
        return ata_read_range(lba0, KZFS_BLOCK_SECTORS, (uint8_t*)buf);
    }
    return ata_read_block4k_legacy(block_num, buf);
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