// tools/mkfs.kyuzenfs.c — Host formatter KyuzenFS V4 (Modul 2).
//
// Membuat image disk dengan layout KyuzenFS V4:
//   Block 0: Superblock | 1..N: Block Bitmap | N+1..M: Inode Bitmap
//   M+1..K: Inode Table | K+1..END: Data Blocks
// Root dir (inode 1) berisi ".", "..", dan "apps" (folder kosong).
//
// Build & pakai (host Linux/WSL/macOS/MinGW):
//   clang -O2 -Wall -o mkfs.kyuzenfs tools/mkfs.kyuzenfs.c
//   ./mkfs.kyuzenfs disk.img            # format disk.img 100MB (default)
//   ./mkfs.kyuzenfs disk.img 512        # ukuran MB kustom
//
// Layout harus konsisten dengan kernel/fs/kyuzenfs_v4.c (layout_compute).
// Struktur on-disk di-duplikasi di sini agar tool standalone (tanpa header
// kernel, tanpa atribut freestanding).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KZFS_MAGIC          0x53465A4Bu
#define KZFS_VERSION        0x00040000u
#define KZFS_BLOCK_SIZE     4096u
#define KZFS_BLOCK_SECTORS  8u
#define KZFS_INODE_SIZE     128u
#define KZFS_INODES_PER_BLOCK (KZFS_BLOCK_SIZE / KZFS_INODE_SIZE)
#define KZFS_NAME_MAX       255u
#define KZFS_NUM_DIRECT_EXTENTS 4u

// Ekor disk yang disisihkan untuk crashdump panic (harus sama dengan
// include/kyuzenfs_v4.h & kernel/fs/kfs_super.c). Tool ini sengaja standalone,
// jadi konstantanya diduplikasi seperti konstanta layout lain di atas.
#define KZFS_CRASHDUMP_SECTORS 8u

#define FLAG_DIR  0x02

struct kzfs_extent {
    uint64_t start_block;
    uint32_t block_count;
};

struct kzfs_superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t _pad0;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint64_t block_bitmap_start;
    uint32_t block_bitmap_blocks;
    uint32_t _pad1;
    uint64_t inode_bitmap_start;
    uint32_t inode_bitmap_blocks;
    uint32_t _pad2;
    uint64_t inode_table_start;
    uint32_t inode_table_blocks;
    uint32_t _pad3;
    uint64_t data_blocks_start;
    uint32_t root_inode;
    uint32_t _pad4;
    uint8_t  reserved[3992];
} __attribute__((packed));

struct kzfs_inode {
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint16_t links_count;
    uint64_t size_bytes;
    uint64_t blocks_used;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    struct kzfs_extent direct[KZFS_NUM_DIRECT_EXTENTS];
    uint64_t indirect_extent_block;
    // WAJIB sama dengan include/kyuzenfs_v4.h: total ukuran TEPAT 128B
    // (KZFS_INODE_SIZE) — slot tabel inode. reserved = 24, bukan 32.
    uint8_t  reserved[24];
} __attribute__((packed));

// Dirent ditulis manual (header 8B + nama) — pola sama dengan kernel.

static void put_bitmap(uint8_t *bm, uint64_t bit) { bm[bit >> 3] |= (uint8_t)(1u << (bit & 7)); }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Pemakaian: %s <disk.img> [ukuran_MB=100]\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    // Prioritas ukuran: argv[2] eksplisit > ukuran file eksisting > default.
    // Ukuran file eksisting PENTING: superblock.total_blocks harus cocok
    // dengan ata_get_total_sectors() saat kernel boot dari disk.img QEMU.
    uint64_t size_mb = (argc >= 3) ? (uint64_t)strtoull(argv[2], NULL, 10) : 0;

    FILE *f = fopen(path, "r+b");
    int existed = (f != NULL);
    if (!existed) f = fopen(path, "w+b");
    if (!f) { perror("fopen"); return 1; }

    if (size_mb == 0 && existed) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long fsz = ftell(f);
            if (fsz > 0) size_mb = (uint64_t)fsz / (1024 * 1024);
        }
        fseek(f, 0, SEEK_SET);
    }
    if (size_mb == 0) size_mb = 100;    // default buat image baru
    if (size_mb < 8)  size_mb = 8;      // minimum masuk akal
    if (size_mb > 2048) size_mb = 2048; // LBA28 guard (alamat sektor 32-bit)

    uint64_t total_sectors = size_mb * 1024 * 1024 / 512;
    // Ekor disk disisihkan untuk crashdump panic (kernel/debug/crashdump.c) — sama
    // seperti format di kernel (kernel/fs/kfs_super.c). Keduanya memakai
    // KZFS_CRASHDUMP_SECTORS dari include/kyuzenfs_v4.h.
    if (total_sectors > KZFS_CRASHDUMP_SECTORS)
        total_sectors -= KZFS_CRASHDUMP_SECTORS;
    uint64_t total_blocks  = total_sectors / KZFS_BLOCK_SECTORS;

    // --- Layout (identik layout_compute di kernel) ---
    uint64_t total_inodes = total_blocks / 16;
    if (total_inodes > 262144) total_inodes = 262144;
    if (total_inodes < 512)    total_inodes = 512;
    uint64_t cap = total_blocks * (KZFS_BLOCK_SIZE / KZFS_INODE_SIZE) / 2;
    if (total_inodes > cap) total_inodes = cap;

    struct kzfs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = KZFS_MAGIC;
    sb.version = KZFS_VERSION;
    sb.block_size = KZFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.total_inodes = (uint32_t)total_inodes;
    sb.block_bitmap_start = 1;
    sb.block_bitmap_blocks = (uint32_t)(((total_blocks + 7) / 8 + KZFS_BLOCK_SIZE - 1) / KZFS_BLOCK_SIZE);
    sb.inode_bitmap_start = sb.block_bitmap_start + sb.block_bitmap_blocks;
    sb.inode_bitmap_blocks = (uint32_t)(((total_inodes + 7) / 8 + KZFS_BLOCK_SIZE - 1) / KZFS_BLOCK_SIZE);
    sb.inode_table_start = sb.inode_bitmap_start + sb.inode_bitmap_blocks;
    sb.inode_table_blocks = (uint32_t)((total_inodes + KZFS_INODES_PER_BLOCK - 1) / KZFS_INODES_PER_BLOCK);
    sb.data_blocks_start = sb.inode_table_start + sb.inode_table_blocks;
    sb.free_blocks = total_blocks - sb.data_blocks_start;
    sb.free_inodes = (uint32_t)total_inodes - 1;   // inode 1 = root
    sb.root_inode = 1;

    // --- Tulis image ---
    // (f sudah terbuka: "r+b" untuk file eksisting — reformat cepat tanpa
    // mengubah ukuran; "w+b" untuk image baru. Block data tak tersentuh
    // dibiarkan apa adanya — semua ditandai bebas di bitmap, pola mkfs
    // quick-format.)
    uint8_t *blk = (uint8_t*)malloc(KZFS_BLOCK_SIZE);
    uint8_t *bm  = (uint8_t*)malloc((size_t)sb.block_bitmap_blocks * KZFS_BLOCK_SIZE);
    uint8_t *ibm = (uint8_t*)malloc((size_t)sb.inode_bitmap_blocks * KZFS_BLOCK_SIZE);
    if (!blk || !bm || !ibm) { fprintf(stderr, "OOM\n"); return 1; }

    // Block 0: superblock
    memset(blk, 0, KZFS_BLOCK_SIZE);
    memcpy(blk, &sb, sizeof(sb));
    fwrite(blk, 1, KZFS_BLOCK_SIZE, f);

    // Block bitmap: metadata (0..data_start-1) dipakai; inode 1 & 2 (root+apps)
    memset(bm, 0, (size_t)sb.block_bitmap_blocks * KZFS_BLOCK_SIZE);
    for (uint64_t b = 0; b < sb.data_blocks_start; b++) put_bitmap(bm, b);
    fwrite(bm, KZFS_BLOCK_SIZE, sb.block_bitmap_blocks, f);

    // Inode bitmap: inode 1 (root) + 2 (apps)
    memset(ibm, 0, (size_t)sb.inode_bitmap_blocks * KZFS_BLOCK_SIZE);
    put_bitmap(ibm, 1);
    put_bitmap(ibm, 2);
    fwrite(ibm, KZFS_BLOCK_SIZE, sb.inode_bitmap_blocks, f);

    // Inode table: inode 1 = root dir, inode 2 = /apps
    uint64_t it_bytes = (uint64_t)sb.inode_table_blocks * KZFS_BLOCK_SIZE;
    uint8_t *it = (uint8_t*)malloc(it_bytes);
    if (!it) { fprintf(stderr, "OOM\n"); return 1; }
    memset(it, 0, it_bytes);

    struct kzfs_inode *root  = (struct kzfs_inode*)(it + 0);                    // inode 1
    struct kzfs_inode *apps  = (struct kzfs_inode*)(it + KZFS_INODE_SIZE);      // inode 2
    root->mode = FLAG_DIR;  root->links_count = 3;
    apps->mode = FLAG_DIR;  apps->links_count = 2;

    // Data: 3 block — root dir block, apps dir block
    uint64_t root_blk = sb.data_blocks_start;
    uint64_t apps_blk = sb.data_blocks_start + 1;
    root->blocks_used = 1; root->size_bytes = KZFS_BLOCK_SIZE;
    apps->blocks_used = 1; apps->size_bytes = KZFS_BLOCK_SIZE;
    root->direct[0].start_block = root_blk; root->direct[0].block_count = 1;
    apps->direct[0].start_block = apps_blk; apps->direct[0].block_count = 1;
    fwrite(it, 1, it_bytes, f);
    free(it);

    // Root dir block: "." (offset 0), ".." (12), "apps" (24).
    // Dirent: ino(4) rec(2) nlen(1) ftype(1) name(...)
    memset(blk, 0, KZFS_BLOCK_SIZE);
    {
        uint32_t ino1 = 1, ino2 = 2;
        uint16_t rec12 = 12;
        uint16_t recapps = (uint16_t)(KZFS_BLOCK_SIZE - 24);
        uint8_t ft = FLAG_DIR;
        // "." @0
        memcpy(blk + 0,  &ino1, 4); memcpy(blk + 4, &rec12, 2);
        blk[6] = 1; blk[7] = ft; blk[8] = '.';
        // ".." @12: header 8B @12..19 (ino@12, rec@16, nlen@18, ftype@19),
        // nama @20..21. (Sebelumnya salah geser +4 → dirent korup.)
        memcpy(blk + 12, &ino1, 4); memcpy(blk + 16, &rec12, 2);
        blk[18] = 2; blk[19] = ft; blk[20] = '.'; blk[21] = '.';
        // "apps" @24 (header 8B di 24..31, nama di 32..35)
        memcpy(blk + 24, &ino2, 4); memcpy(blk + 28, &recapps, 2);
        blk[30] = 4; blk[31] = ft;
        memcpy(blk + 32, "apps", 4);
    }
    fwrite(blk, 1, KZFS_BLOCK_SIZE, f);

    // Apps dir block: "." (offset 0), ".." (12, rec = sisa block)
    memset(blk, 0, KZFS_BLOCK_SIZE);
    {
        uint32_t ino2 = 2, ino1 = 1;
        uint16_t rec1 = 12;
        uint16_t rec2 = (uint16_t)(KZFS_BLOCK_SIZE - 12);
        uint8_t ft = FLAG_DIR;
        memcpy(blk, &ino2, 4); memcpy(blk + 4, &rec1, 2);
        blk[6] = 1; blk[7] = ft; blk[8] = '.';
        // ".." @12: header 8B @12..19, nama @20..21 (layout dirent standar).
        memcpy(blk + 12, &ino1, 4); memcpy(blk + 16, &rec2, 2);
        blk[18] = 2; blk[19] = ft; blk[20] = '.'; blk[21] = '.';
    }
    fwrite(blk, 1, KZFS_BLOCK_SIZE, f);

    free(blk); free(bm); free(ibm);

    // Sisa image: nol (seek + tulis 1 block terakhir agar ukuran pas).
    uint64_t written = ftell(f);
    uint64_t target = total_sectors * 512;
    if (written > target) { fprintf(stderr, "Image terlalu kecil untuk metadata!\n"); fclose(f); return 1; }
    if (fseek(f, (long)(target - 512), SEEK_SET) != 0) { perror("fseek"); fclose(f); return 1; }
    uint8_t *tail = (uint8_t*)malloc(512);
    memset(tail, 0, 512);
    fwrite(tail, 1, 512, f);
    free(tail);
    fclose(f);

    printf("[mkfs] %s: %llu MB, %llu block, %u inode, data@%llu (root=1, /apps=2)\n",
           path, (unsigned long long)size_mb, (unsigned long long)total_blocks,
           (unsigned)total_inodes, (unsigned long long)sb.data_blocks_start);
    return 0;
}
