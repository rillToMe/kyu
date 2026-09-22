// kernel/fs/kfs_super.c — KyuzenFS V4: state global, superblock, layout,
// mount, format, dan sinkronisasi global (split dari kyuzenfs_v4.c).
//
// File ini PEMILIK state global FS (sb_cache, bitmap RAM, fs_lock, dll).
// Kontrak antar-modul: kernel/fs/kfs_internal.h.

#include "kfs_internal.h"

// =====================================================================
// STATE GLOBAL FS
// =====================================================================
struct kzfs_superblock sb_cache;        // superblock di RAM
int      fs_mounted = 0;

uint8_t *bm_mem = NULL;                 // Block Bitmap penuh di RAM
uint32_t bm_blocks = 0;
int      bm_dirty = 0;

uint8_t *ibm_mem = NULL;                // Inode Bitmap penuh di RAM
uint32_t ibm_blocks = 0;
int      ibm_dirty = 0;

uint64_t it_block0 = 0;                 // block pertama Inode Table
uint32_t root_ino  = 1;

uint64_t data_start = 0;                // block pertama area data

spinlock_t fs_lock = SPINLOCK_INIT;

// =====================================================================
// SUPERBLOCK I/O (caller holds fs_lock)
// =====================================================================
int sb_load(void) {
    struct block_buffer *b = bcache_read(0);
    if (!b) return KZFS_EIO;
    memcpy(&sb_cache, b->data, sizeof(sb_cache));
    bcache_release(b);
    if (sb_cache.magic != (uint32_t)KZFS_MAGIC) return KZFS_EINVAL;
    return KZFS_EOK;
}

int sb_save(void) {
    struct block_buffer *b = bcache_get(0);
    if (!b) return KZFS_EIO;
    memcpy(b->data, &sb_cache, sizeof(sb_cache));
    bcache_mark_dirty(b);
    bcache_release(b);
    return KZFS_EOK;
}

// Hitung layout on-disk untuk disk berukuran total_blocks.
static void layout_compute(uint64_t total_blocks) {
    uint64_t total_inodes = total_blocks / 16;                  // ~1 inode per 64KB
    if (total_inodes > 262144) total_inodes = 262144;
    if (total_inodes < 512)    total_inodes = 512;
    uint64_t cap = total_blocks * (KZFS_BLOCK_SIZE / KZFS_INODE_SIZE) / 2;
    if (total_inodes > cap) total_inodes = cap;

    sb_cache.magic            = (uint32_t)KZFS_MAGIC;
    sb_cache.version          = KZFS_VERSION;
    sb_cache.block_size       = KZFS_BLOCK_SIZE;
    sb_cache.total_blocks     = total_blocks;
    sb_cache.total_inodes     = (uint32_t)total_inodes;

    sb_cache.block_bitmap_start = 1;
    uint64_t bm_bytes = (total_blocks + 7) / 8;
    sb_cache.block_bitmap_blocks = (uint32_t)((bm_bytes + KZFS_BLOCK_SIZE - 1) / KZFS_BLOCK_SIZE);

    sb_cache.inode_bitmap_start = sb_cache.block_bitmap_start + sb_cache.block_bitmap_blocks;
    uint64_t ib_bytes = (total_inodes + 7) / 8;
    sb_cache.inode_bitmap_blocks = (uint32_t)((ib_bytes + KZFS_BLOCK_SIZE - 1) / KZFS_BLOCK_SIZE);

    sb_cache.inode_table_start = sb_cache.inode_bitmap_start + sb_cache.inode_bitmap_blocks;
    sb_cache.inode_table_blocks = (uint32_t)((total_inodes + KZFS_INODES_PER_BLOCK - 1) / KZFS_INODES_PER_BLOCK);

    sb_cache.data_blocks_start = sb_cache.inode_table_start + sb_cache.inode_table_blocks;
    sb_cache.free_blocks       = total_blocks - sb_cache.data_blocks_start;
    sb_cache.free_inodes       = (uint32_t)total_inodes - 1;   // inode 1 = root
    sb_cache.root_inode        = 1;
}

// Sinkronkan global layout dari sb_cache — WAJIB dipanggil sebelum operasi
// disk apa pun (format maupun mount). Bug lama: hanya mount yang set global
// ini, sehingga selama format ino_disk_write menulis inode ke block 0
// (menimpa superblock) dan isi disk jadi sampah.
static void layout_globals_load(void) {
    bm_blocks  = sb_cache.block_bitmap_blocks;
    ibm_blocks = sb_cache.inode_bitmap_blocks;
    it_block0  = sb_cache.inode_table_start;
    data_start = sb_cache.data_blocks_start;
    root_ino   = sb_cache.root_inode ? sb_cache.root_inode : 1;
}

// Muat bitmap dari disk ke RAM (setelah superblock valid).
int mount_load_bitmaps(void) {
    layout_globals_load();

    if (bm_mem) kfree(bm_mem);
    if (ibm_mem) kfree(ibm_mem);
    bm_mem  = (uint8_t*)kmalloc((uint32_t)(bm_blocks * KZFS_BLOCK_SIZE));
    ibm_mem = (uint8_t*)kmalloc((uint32_t)(ibm_blocks * KZFS_BLOCK_SIZE));
    if (!bm_mem || !ibm_mem) return KZFS_ENOMEM;

    for (uint32_t i = 0; i < bm_blocks; i++) {
        struct block_buffer *b = bcache_read(sb_cache.block_bitmap_start + i);
        if (!b) return KZFS_EIO;
        memcpy(bm_mem + i * KZFS_BLOCK_SIZE, b->data, KZFS_BLOCK_SIZE);
        bcache_release(b);
    }
    for (uint32_t i = 0; i < ibm_blocks; i++) {
        struct block_buffer *b = bcache_read(sb_cache.inode_bitmap_start + i);
        if (!b) return KZFS_EIO;
        memcpy(ibm_mem + i * KZFS_BLOCK_SIZE, b->data, KZFS_BLOCK_SIZE);
        bcache_release(b);
    }
    bm_dirty = ibm_dirty = 0;
    fs_mounted = 1;
    return KZFS_EOK;
}

// =====================================================================
// BITMAP FLUSH (caller holds fs_lock)
// =====================================================================
int bm_flush(void) {
    if (!bm_dirty) return KZFS_EOK;
    for (uint32_t i = 0; i < bm_blocks; i++) {
        struct block_buffer *b = bcache_get(sb_cache.block_bitmap_start + i);
        if (!b) return KZFS_EIO;
        memcpy(b->data, bm_mem + i * KZFS_BLOCK_SIZE, KZFS_BLOCK_SIZE);
        bcache_mark_dirty(b);
        bcache_release(b);
    }
    bm_dirty = 0;
    return KZFS_EOK;
}

int ibm_flush(void) {
    if (!ibm_dirty) return KZFS_EOK;
    for (uint32_t i = 0; i < ibm_blocks; i++) {
        struct block_buffer *b = bcache_get(sb_cache.inode_bitmap_start + i);
        if (!b) return KZFS_EIO;
        memcpy(b->data, ibm_mem + i * KZFS_BLOCK_SIZE, KZFS_BLOCK_SIZE);
        bcache_mark_dirty(b);
        bcache_release(b);
    }
    ibm_dirty = 0;
    return KZFS_EOK;
}

// =====================================================================
// FORMAT & MOUNT
// =====================================================================
static int kfs_format_locked(void) {
    uint32_t total_sectors = ata_get_total_sectors();
    if (total_sectors == 0) total_sectors = 204800;   // fallback QEMU
    // Sisihkan ekor disk untuk crashdump panic (kernel/debug/crashdump.c).
    if (total_sectors > KZFS_CRASHDUMP_SECTORS)
        total_sectors -= KZFS_CRASHDUMP_SECTORS;
    uint64_t total_blocks = total_sectors / KZFS_BLOCK_SECTORS;
    layout_compute(total_blocks);
    layout_globals_load();     // PENTING: inode/bitmap write butuh ini sejak block pertama

    if (bm_mem) kfree(bm_mem);
    if (ibm_mem) kfree(ibm_mem);
    bm_mem  = (uint8_t*)kmalloc((uint32_t)(sb_cache.block_bitmap_blocks * KZFS_BLOCK_SIZE));
    ibm_mem = (uint8_t*)kmalloc((uint32_t)(sb_cache.inode_bitmap_blocks * KZFS_BLOCK_SIZE));
    if (!bm_mem || !ibm_mem) return KZFS_ENOMEM;
    memset(bm_mem,  0, (size_t)(sb_cache.block_bitmap_blocks * KZFS_BLOCK_SIZE));
    memset(ibm_mem, 0, (size_t)(sb_cache.inode_bitmap_blocks * KZFS_BLOCK_SIZE));
    for (uint64_t b = 0; b < sb_cache.data_blocks_start; b++) bm_set(bm_mem, b);
    bm_set(ibm_mem, 1);                          // inode 1 = root
    bm_dirty = ibm_dirty = 1;
    fs_mounted = 1;

    // Kosongkan inode cache (semua inode lama tidak valid lagi).
    ino_cache_reset();

    // Root dir: inode 1, block data = "." ".." "apps".
    ino_entry_t *re = ino_cache_slot();
    if (!re) return KZFS_ENOMEM;
    memset(&re->ino, 0, sizeof(re->ino));
    re->ino.ino = 1; re->ino.mode = KZFS_INODE_FLAG_DIR;
    re->ino.links_count = 3;                     // ".", ".." apps dir, entri di /
    re->ino.refcount = 1;
    re->used = 1;
    re->next = NULL;
    ino_cache_insert_nolock(re);
    (void)ino_sync_nolock(&re->ino);

    // /apps dulu (butuh parent root sudah ada sebagai inode).
    ino_entry_t *ae = NULL;
    int rc = create_child(&re->ino, "apps", KZFS_INODE_FLAG_DIR, &ae);
    if (rc != KZFS_EOK) { re->ino.refcount = 0; ino_cache_unlink(re); return rc; }

    // Tulis block data root: ".", "..", "apps".
    uint8_t *blk = (uint8_t*)kmalloc(KZFS_BLOCK_SIZE);
    if (!blk) { re->ino.refcount = 0; ino_cache_unlink(re); return KZFS_ENOMEM; }
    memset(blk, 0, KZFS_BLOCK_SIZE);
    struct kzfs_dir_entry dot;
    memset(&dot, 0, sizeof(dot));
    dot.inode_num = 1; dot.rec_len = (uint16_t)((8 + 1 + 3) & ~3u);
    dot.name_len = 1; dot.file_type = KZFS_INODE_FLAG_DIR; dot.name[0] = '.';
    memcpy(blk, &dot, 8 + 1);
    struct kzfs_dir_entry dotdot = dot;
    dotdot.rec_len = (uint16_t)((8 + 2 + 3) & ~3u);
    dotdot.name_len = 2; dotdot.name[1] = '.';
    memcpy(blk + dot.rec_len, &dotdot, 8 + 2);
    struct kzfs_dir_entry apps;
    memset(&apps, 0, sizeof(apps));
    apps.inode_num = ae->ino.ino;
    apps.rec_len = (uint16_t)(KZFS_BLOCK_SIZE - dot.rec_len - dotdot.rec_len);
    apps.name_len = 4; apps.file_type = KZFS_INODE_FLAG_DIR;
    apps.name[0]='a'; apps.name[1]='p'; apps.name[2]='p'; apps.name[3]='s';
    memcpy(blk + dot.rec_len + dotdot.rec_len, &apps, 8 + 4);
    uint32_t w = 0;
    (void)blk_write_file(&re->ino, 0, blk, KZFS_BLOCK_SIZE, &w);
    kfree(blk);
    (void)ino_sync_nolock(&re->ino);
    re->ino.refcount = 0;                        // root dipakai langsung via &re->ino
    re->ino.dirty = 0;
    ae->ino.refcount = 0;
    ae->ino.dirty = 0;

    (void)bm_flush();
    (void)ibm_flush();
    (void)sb_save();
    bcache_flush_all();
    return KZFS_EOK;
}

void kfs_init(void) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);

    if (bcache_init() != 0) {
        kprint("[KZFS4] FATAL: bcache init gagal (heap)\n");
        spinlock_unlock_irqrestore(&fs_lock, flags);
        return;
    }
    ino_cache_reset();

    int rc = sb_load();
    if (rc == KZFS_EINVAL) {
        kprint("[KZFS4] Disk belum terformat V4 — memformat...\n");
        rc = kfs_format_locked();
        kprint(rc == KZFS_EOK ? "[KZFS4] Disk diformat KyuzenFS V4 (extent-based)\n"
                              : "[KZFS4] FATAL: format gagal\n");
        spinlock_unlock_irqrestore(&fs_lock, flags);
        return;
    }
    if (rc != KZFS_EOK) {
        kprint("[KZFS4] FATAL: gagal baca superblock\n");
        spinlock_unlock_irqrestore(&fs_lock, flags);
        return;
    }
    rc = mount_load_bitmaps();
    kprint(rc == KZFS_EOK ? "[KZFS4] Mount OK\n" : "[KZFS4] FATAL: mount bitmap\n");
    spinlock_unlock_irqrestore(&fs_lock, flags);
}

void kfs_format(void) {
    // Format eksplisit (syscall 5, root only).
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    fs_mounted = 0;
    sb_cache.magic = 0;
    int rc = kfs_format_locked();
    spinlock_unlock_irqrestore(&fs_lock, flags);
    kprint(rc == KZFS_EOK ? "[KZFS4] Disk diformat KyuzenFS V4\n"
                          : "[KZFS4] FATAL: format gagal\n");
}

// =====================================================================
// STATISTIK & SYNC GLOBAL
// =====================================================================
int kfs_v4_is_mounted(void) { return fs_mounted; }

void kfs_v4_get_stats(uint64_t *total_blocks, uint64_t *free_blocks,
                      uint32_t *total_inodes, uint32_t *free_inodes) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    if (total_blocks)  *total_blocks  = sb_cache.total_blocks;
    if (free_blocks)   *free_blocks   = sb_cache.free_blocks;
    if (total_inodes)  *total_inodes  = sb_cache.total_inodes;
    if (free_inodes)   *free_inodes   = sb_cache.free_inodes;
    spinlock_unlock_irqrestore(&fs_lock, flags);
}

// Sinkronisasi global: metadata + block dirty → disk.
// Dipanggil timer berkala dan shutdown.
void kfs_sync_all(void) {
    if (!fs_mounted) return;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    (void)bm_flush();
    (void)ibm_flush();
    (void)sb_save();
    spinlock_unlock_irqrestore(&fs_lock, flags);
    bcache_flush_all();
}

// Padanan jalur panic (lihat include/kyuzenfs.h): semua lock dicoba tanpa
// menunggu. Kalau fs_lock tidak bisa diambil, tidak ada yang ditulis dan kita
// menyerah lebih awal — termasuk TIDAK memanggil bcache_flush_all_try(),
// karena menulis block cache sambil metadata FS basi lebih membingungkan
// daripada melewatkannya (data yang sudah ter-flush tetap utuh).
int kfs_sync_all_try(void) {
    if (!fs_mounted) return 0;

    uint64_t flags;
    if (!spinlock_try_lock_irqsave(&fs_lock, &flags)) return 0;
    (void)bm_flush();
    (void)ibm_flush();
    (void)sb_save();
    spinlock_unlock_irqrestore(&fs_lock, flags);

    return bcache_flush_all_try();
}

void kfs_v4_cache_stats(uint64_t *hit, uint64_t *miss) {
    if (hit)  *hit  = bcache_hit_count();
    if (miss) *miss = bcache_miss_count();
}
