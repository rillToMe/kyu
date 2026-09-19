// kernel/fs/kfs_extent.c — KyuzenFS V4: extent engine + block-level file
// I/O + truncate (split dari kyuzenfs_v4.c).
//
// Offset logis → block fisik diterjemahkan lewat daftar extent
// [start_block, block_count] milik inode (4 direct + indirect block).
// Semua baca/tulis block fisik lewat bcache — in-place, streaming.
// Caller holds fs_lock.

#include "kfs_internal.h"

// =====================================================================
// EXTENT ENGINE — offset logis → block fisik
// =====================================================================
// Baca entri extent ke-N (N >= 4 → indirect block). Caller holds lock.
int ext_get_nolock(kzfs_v4_inode_mem_t *in, uint32_t n,
                   struct kzfs_extent *out) {
    if (n < KZFS_NUM_DIRECT_EXTENTS) { *out = in->direct[n]; return KZFS_EOK; }
    uint32_t k = n - KZFS_NUM_DIRECT_EXTENTS;
    if (k >= KZFS_EXTS_PER_IND_BLOCK) return KZFS_EINVAL;
    if (!in->indirect_extent_block) { out->start_block = 0; out->block_count = 0; return KZFS_EOK; }
    struct block_buffer *b = bcache_read(in->indirect_extent_block);
    if (!b) return KZFS_EIO;
    memcpy(out, b->data + k * 12, 12);
    bcache_release(b);
    return KZFS_EOK;
}

// Tulis entri extent ke-N (alokasi indirect block bila perlu). Caller holds lock.
int ext_set_nolock(kzfs_v4_inode_mem_t *in, uint32_t n,
                   const struct kzfs_extent *e) {
    if (n < KZFS_NUM_DIRECT_EXTENTS) {
        in->direct[n] = *e;
        in->dirty = 1;
        return KZFS_EOK;
    }
    uint32_t k = n - KZFS_NUM_DIRECT_EXTENTS;
    if (k >= KZFS_EXTS_PER_IND_BLOCK) return KZFS_EINVAL;
    if (!in->indirect_extent_block) {
        uint32_t got;
        uint64_t start = blk_alloc_run_nolock(1, &got);
        if (!start) return KZFS_ENOSPC;
        in->indirect_extent_block = start;
        in->blocks_used += 1;
        in->dirty = 1;
        struct block_buffer *zb = bcache_get(start);   // zero indirect block
        if (!zb) return KZFS_EIO;
        memset(zb->data, 0, KZFS_BLOCK_SIZE);
        bcache_mark_dirty(zb);
        bcache_release(zb);
    }
    struct block_buffer *b = bcache_read(in->indirect_extent_block);
    if (!b) return KZFS_EIO;
    memcpy(b->data + k * 12, e, 12);
    bcache_mark_dirty(b);
    bcache_release(b);
    return KZFS_EOK;
}

// Terjemahan offset logis → block fisik. Return 1 + *phys_out bila
// logical_block terpetakan; 0 bila lubang/EOF. Caller holds lock.
uint32_t kzfs_get_phys_block(kzfs_v4_inode_mem_t *inode,
                             uint64_t logical_block,
                             uint64_t *phys_out) {
    uint64_t cursor = 0;
    struct kzfs_extent e;
    for (uint32_t n = 0; n < KZFS_MAX_EXTENTS; n++) {
        if (ext_get_nolock(inode, n, &e) != KZFS_EOK) break;
        if (e.block_count == 0) {
            if (n < KZFS_NUM_DIRECT_EXTENTS) continue;   // slot direct kosong
            break;                                        // akhir list indirect
        }
        if (logical_block < cursor + e.block_count) {
            *phys_out = e.start_block + (logical_block - cursor);
            return 1;
        }
        cursor += e.block_count;
    }
    return 0;
}

// Tambah extent (start,count) di slot kosong pertama; merge bila kontigu
// dengan extent terakhir. Caller holds fs_lock.
int ext_append_nolock(kzfs_v4_inode_mem_t *in,
                      uint64_t start, uint32_t count) {
    struct kzfs_extent e;
    for (uint32_t n = 0; n < KZFS_MAX_EXTENTS; n++) {
        if (ext_get_nolock(in, n, &e) != KZFS_EOK) return KZFS_EIO;
        if (e.block_count == 0) {
            e.start_block = start; e.block_count = count;
            return ext_set_nolock(in, n, &e);
        }
        if (e.start_block + e.block_count == start) {   // merge kontigu
            e.block_count += count;
            return ext_set_nolock(in, n, &e);
        }
    }
    return KZFS_EINVAL;
}

// Perbesar cakupan block file sampai logical_block (alokasi + append).
// Caller holds fs_lock. Return 0 / error.
int grow_to_nolock(kzfs_v4_inode_mem_t *in, uint64_t logical_block) {
    uint64_t cursor = 0;
    struct kzfs_extent e;
    for (uint32_t n = 0; n < KZFS_MAX_EXTENTS; n++) {
        if (ext_get_nolock(in, n, &e) != KZFS_EOK) return KZFS_EIO;
        if (e.block_count == 0) break;
        cursor += e.block_count;
    }
    if (logical_block < cursor) return KZFS_EOK;     // sudah tercakup

    while (logical_block >= cursor) {
        uint64_t need = logical_block - cursor + 1;
        uint32_t want = (need > 64) ? 64 : (uint32_t)need;
        uint32_t got = 0;
        uint64_t start = blk_alloc_run_nolock(want, &got);
        if (!start) return KZFS_ENOSPC;
        int rc = ext_append_nolock(in, start, got);
        if (rc != KZFS_EOK) { blk_free_run_nolock(start, got); return rc; }
        in->blocks_used += got;
        in->dirty = 1;
        cursor += got;
    }
    return KZFS_EOK;
}

// =====================================================================
// BLOCK-LEVEL READ/WRITE (caller holds fs_lock)
// =====================================================================
int blk_read_file(kzfs_v4_inode_mem_t *in, uint64_t file_off,
                  void *buf, uint32_t count, uint32_t *nread) {
    *nread = 0;
    if (file_off >= in->size_bytes) return KZFS_EOK;              // EOF
    if (count > in->size_bytes - file_off)
        count = (uint32_t)(in->size_bytes - file_off);

    while (count > 0) {
        uint64_t lb    = file_off / KZFS_BLOCK_SIZE;
        uint32_t boff  = (uint32_t)(file_off % KZFS_BLOCK_SIZE);
        uint32_t chunk = KZFS_BLOCK_SIZE - boff;
        if (chunk > count) chunk = count;

        uint64_t phys = 0;
        if (!kzfs_get_phys_block(in, lb, &phys)) {
            memset((uint8_t*)buf + *nread, 0, chunk);   // lubang → nol
        } else {
            struct block_buffer *b = bcache_read(phys);
            if (!b) return KZFS_EIO;
            memcpy((uint8_t*)buf + *nread, b->data + boff, chunk);
            bcache_release(b);
        }
        *nread   += chunk;
        file_off += chunk;
        count    -= chunk;
    }
    return KZFS_EOK;
}

int blk_write_file(kzfs_v4_inode_mem_t *in, uint64_t file_off,
                   const void *buf, uint32_t count, uint32_t *nwritten) {
    *nwritten = 0;
    while (count > 0) {
        uint64_t lb    = file_off / KZFS_BLOCK_SIZE;
        uint32_t boff  = (uint32_t)(file_off % KZFS_BLOCK_SIZE);
        uint32_t chunk = KZFS_BLOCK_SIZE - boff;
        if (chunk > count) chunk = count;

        uint64_t phys = 0;
        if (!kzfs_get_phys_block(in, lb, &phys)) {
            int rc = grow_to_nolock(in, lb);
            if (rc != KZFS_EOK) return rc;
            if (!kzfs_get_phys_block(in, lb, &phys)) return KZFS_EIO;
        }
        // Full-block overwrite pakai bcache_get (tanpa baca disk);
        // partial read-modify-write pakai bcache_read. IN-PLACE.
        struct block_buffer *b = (boff == 0 && chunk == KZFS_BLOCK_SIZE)
                                 ? bcache_get(phys) : bcache_read(phys);
        if (!b) return KZFS_EIO;
        memcpy(b->data + boff, (const uint8_t*)buf + *nwritten, chunk);
        bcache_mark_dirty(b);
        bcache_release(b);

        *nwritten += chunk;
        file_off  += chunk;
        count     -= chunk;
        if (file_off > in->size_bytes) in->size_bytes = file_off;
        in->dirty = 1;
    }
    return KZFS_EOK;
}

// Truncate (mengecil; perbesar = sparse). Caller holds fs_lock.
int truncate_nolock(kzfs_v4_inode_mem_t *in, uint64_t new_size) {
    uint64_t new_lb = (new_size == 0) ? 0 : (new_size - 1) / KZFS_BLOCK_SIZE + 1;
    uint64_t cursor = 0;
    struct kzfs_extent e;

    for (uint32_t n = 0; n < KZFS_MAX_EXTENTS; n++) {
        if (ext_get_nolock(in, n, &e) != KZFS_EOK) return KZFS_EIO;
        if (e.block_count == 0) break;
        uint64_t ext_first = cursor;
        cursor += e.block_count;

        if (new_lb <= ext_first) {
            blk_free_run_nolock(e.start_block, e.block_count);
            in->blocks_used -= e.block_count;
            struct kzfs_extent zero = {0, 0};
            (void)ext_set_nolock(in, n, &zero);
        } else if (new_lb < cursor) {
            uint32_t keep = (uint32_t)(new_lb - ext_first);
            blk_free_run_nolock(e.start_block + keep, e.block_count - keep);
            in->blocks_used -= (e.block_count - keep);
            e.block_count = keep;
            (void)ext_set_nolock(in, n, &e);
        }
    }
    // Indirect extent block dilepas bila tidak ada lagi extent indirect.
    if (in->indirect_extent_block) {
        int any = 0;
        for (uint32_t k = 0; k < KZFS_EXTS_PER_IND_BLOCK; k++) {
            if (ext_get_nolock(in, KZFS_NUM_DIRECT_EXTENTS + k, &e) != KZFS_EOK) break;
            if (e.block_count != 0) { any = 1; break; }
        }
        if (!any) {
            blk_free_run_nolock(in->indirect_extent_block, 1);
            in->blocks_used -= 1;
            in->indirect_extent_block = 0;
            in->dirty = 1;
        }
    }
    in->size_bytes = new_size;
    in->dirty = 1;
    return KZFS_EOK;
}

// Bebaskan seluruh extent milik inode (saat inode dihapus).
void inode_free_all_extents(kzfs_v4_inode_mem_t *in) {
    struct kzfs_extent e;
    for (uint32_t n = 0; n < KZFS_MAX_EXTENTS; n++) {
        if (ext_get_nolock(in, n, &e) != KZFS_EOK) break;
        if (e.block_count == 0) {
            if (n >= KZFS_NUM_DIRECT_EXTENTS) break;
            continue;
        }
        blk_free_run_nolock(e.start_block, e.block_count);
        struct kzfs_extent zero = {0, 0};
        (void)ext_set_nolock(in, n, &zero);
    }
    if (in->indirect_extent_block) {
        blk_free_run_nolock(in->indirect_extent_block, 1);
        in->indirect_extent_block = 0;
    }
    in->blocks_used = 0;
    in->size_bytes = 0;
    in->dirty = 1;
}
