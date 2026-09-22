#ifndef KYUZENFS_V4_H
#define KYUZENFS_V4_H

// =====================================================================
// KyuzenFS V4 — extent-based, block-cached filesystem (on-disk format).
//
// Perubahan fundamental dari V3 (FAT32-like, whole-file RAM buffer):
//   * Alokasi dalam Block 4KB (8 x 512B sektor) — bukan sektor tunggal.
//   * File = daftar EXTENT [start_block, block_count] — bukan FAT chain.
//   * Inode 128B dengan 4 extent langsung + 1 indirect extent block.
//   * Direktori = file berisi kzfs_dir_entry dengan rec_len variabel.
//   * Semua I/O lewat Block Cache 4KB (bcache) + in-place write.
//
// Semua struktur on-disk packed; field fixed-width. Offset byte dihitung
// dari LBA (512B): byte_addr = block * 4096, LBA = block * 8.
// =====================================================================

#include <stdint.h>

#define KZFS_MAGIC   0x53465A4BULL   // "KZFS" Little-Endian
#define KZFS_VERSION 0x00040000      // v4.0

#define KZFS_BLOCK_SIZE        4096
#define KZFS_BLOCK_SECTORS     (KZFS_BLOCK_SIZE / 512)   // 8 sektor per block
#define KZFS_SECTOR_SIZE       512
#define KZFS_INODE_SIZE        128
#define KZFS_INODES_PER_BLOCK  (KZFS_BLOCK_SIZE / KZFS_INODE_SIZE)  // 32
#define KZFS_NAME_MAX          255
#define KZFS_NUM_DIRECT_EXTENTS 4

// Sektor TERAKHIR disk disisihkan untuk crashdump panic (kernel/debug/crashdump.c,
// include/crashdump.h). FS — baik mkfs host maupun format di kernel — BERHENTI
// sebelum area ini, jadi snapshot panic tidak pernah menimpa data file.
// Angka ini HARUS sama di tools/mkfs.kyuzenfs.c dan kernel/fs/kfs_super.c
// (keduanya memakai makro ini, jangan hardcode).
#define KZFS_CRASHDUMP_SECTORS 8      // 4KB = batas CRASHDUMP_MAX_BYTES

// --- SUPERBLOCK (Block 0) ---
struct kzfs_superblock {
    uint32_t magic;               // 0x53465A4B
    uint32_t version;             // 0x00040000 (v4.0)
    uint32_t block_size;          // 4096
    uint32_t _pad0;               // align 8 (was implicit hole in spec draft)
    uint64_t total_blocks;        // Total block pada disk
    uint64_t free_blocks;         // Block tersisa
    uint32_t total_inodes;        // Total inode
    uint32_t free_inodes;         // Inode tersisa

    // Layout Offsets (indeks BLOCK, bukan sektor)
    uint64_t block_bitmap_start;  // Block indeks awal Block Bitmap
    uint32_t block_bitmap_blocks; // Jumlah block untuk Block Bitmap
    uint32_t _pad1;
    uint64_t inode_bitmap_start;  // Block indeks awal Inode Bitmap
    uint32_t inode_bitmap_blocks; // Jumlah block untuk Inode Bitmap
    uint32_t _pad2;
    uint64_t inode_table_start;   // Block indeks awal Inode Table
    uint32_t inode_table_blocks;  // Jumlah block untuk Inode Table
    uint32_t _pad3;
    uint64_t data_blocks_start;   // Block indeks awal Data Area

    uint32_t root_inode;          // Root dir inode (biasanya 1)
    uint32_t _pad4;
    uint8_t  reserved[3992];      // Padding ke 4096 Bytes (offset 104..4096)
} __attribute__((packed));

// --- EXTENT STRUCTURE (12 Bytes) ---
struct kzfs_extent {
    uint64_t start_block;         // Block awal di area data
    uint32_t block_count;         // Jumlah block kontigu (0 = slot kosong)
} __attribute__((packed));

// --- INODE FLAG ---
#define KZFS_INODE_FLAG_FILE 0x01
#define KZFS_INODE_FLAG_DIR  0x02

// --- INODE STRUCTURE (128 Bytes) ---
struct kzfs_inode {
    uint16_t mode;                // Type & Flags (KZFS_INODE_FLAG_*)
    uint16_t uid;
    uint16_t gid;
    uint16_t links_count;
    uint64_t size_bytes;          // Ukuran file aktual dalam byte
    uint64_t blocks_used;         // Total block terpakai

    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;

    struct kzfs_extent direct[KZFS_NUM_DIRECT_EXTENTS]; // 4 Extents x 12B = 48B (offset 48..96)
    uint64_t indirect_extent_block;                     // Pointer ke indirect extent block (offset 96..104)

    uint8_t  reserved[24];        // Padding ke TEPAT 128 Bytes (104 + 24)
} __attribute__((packed));

// --- DIRECTORY ENTRY STRUCTURE ---
struct kzfs_dir_entry {
    uint32_t inode_num;           // Target inode (0 = deleted)
    uint16_t rec_len;             // Record length (ke entri berikutnya; pad 4)
    uint8_t  name_len;            // String length (tanpa NUL di on-disk)
    uint8_t  file_type;           // 1 = File, 2 = Directory
    char     name[KZFS_NAME_MAX]; // Nama (di-disk diikuti padding rec_len)
} __attribute__((packed));

// Ukuran minimal entri direktori on-disk (header tanpa nama, pad ke 4)
#define KZFS_DIRENT_MIN_REC  ((uint16_t)(8 + 4))   // 12: header 8B + nama minimal 4B pad
// Entri "." dan ".." (root berisi keduanya, subfolder mewarisi pola ext2)
#define KZFS_DIRENT_MAX_REC  ((uint16_t)(8 + KZFS_NAME_MAX))

// =====================================================================
// In-memory (runtime) inode — metadata panas di RAM, tidak pernah
// dimodifikasi langsung sebagai on-disk struct.
// =====================================================================
typedef struct kzfs_v4_inode_mem {
    uint32_t ino;                 // nomor inode (1-based)
    uint16_t mode;                // KZFS_INODE_FLAG_FILE / _DIR
    uint16_t links_count;
    uint64_t size_bytes;
    uint64_t blocks_used;

    struct kzfs_extent direct[KZFS_NUM_DIRECT_EXTENTS];
    uint64_t indirect_extent_block;

    int      dirty;               // metadata perlu ditulis balik
    int      refcount;            // vnode aktif yang memegang inode ini
} kzfs_v4_inode_mem_t;

// Error code POSIX-style (kernel freestanding — definisi lokal).
#define KZFS_EOK     0
#define KZFS_ENOENT  (-2)
#define KZFS_EIO     (-5)
#define KZFS_ENOSPC  (-28)
#define KZFS_EINVAL  (-22)
#define KZFS_EEXIST  (-17)
#define KZFS_ENOTDIR (-20)
#define KZFS_EISDIR  (-21)
#define KZFS_ENOMEM  (-12)
#define KZFS_ENAMETOOLONG (-36)
#define KZFS_EBADF   (-9)

#endif // KYUZENFS_V4_H
