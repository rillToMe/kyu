// kernel/fs/kfs_balloc.c — KyuzenFS V4: bitmap helper + alokasi/bebas
// inode & block (split dari kyuzenfs_v4.c).
//
// Semua fungsi di sini mengubah BITMAP DI RAM saja; penulisan ke disk
// dilakukan pemanggil via bm_flush()/ibm_flush(). Caller holds fs_lock.

#include "kfs_internal.h"

// =====================================================================
// BITMAP HELPERS (caller holds fs_lock)
// =====================================================================
int bm_test(const uint8_t *m, uint64_t bit) {
    return (m[bit >> 3] >> (bit & 7)) & 1;
}

void bm_set(uint8_t *m, uint64_t bit) {
    m[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

void bm_clear(uint8_t *m, uint64_t bit) {
    m[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

// Cari bit 0 mulai `from`; return bit, atau -1 bila penuh.
static int64_t bm_alloc_bit(uint8_t *m, uint64_t total_bits, uint64_t from) {
    for (uint64_t b = from; b < total_bits; b++) {
        if (!bm_test(m, b)) { bm_set(m, b); return (int64_t)b; }
    }
    return -1;
}

// =====================================================================
// ALOKASI INODE (caller holds fs_lock)
// =====================================================================
int32_t ino_alloc_nolock(void) {
    int64_t bit = bm_alloc_bit(ibm_mem, sb_cache.total_inodes, 1);  // 0 reserved
    if (bit < 0) return -1;
    ibm_dirty = 1;
    sb_cache.free_inodes--;
    return (int32_t)bit;
}

// Bebaskan nomor inode: bitmap dikosongkan SEKARANG (nomor boleh dipakai
// ulang), tapi entry cache yang masih dipinang (vnode terbuka pada file
// yang dihapus) hanya ditandai dead — dibuang saat referensi terakhir
// lepas. ino_cache_find melewati entry dead, jadi nomor yang dipakai
// ulang tidak pernah membaca metadata basi.
void ino_free_nolock(uint32_t ino) {
    if (ino == 0 || ino > sb_cache.total_inodes) return;
    ino_entry_t *e = ino_cache_find(ino);
    if (e) {
        e->dead = 1;
        e->ino.dirty = 0;
        if (e->ino.refcount == 0) ino_cache_unlink(e);
    }
    bm_clear(ibm_mem, ino);
    ibm_dirty = 1;
    sb_cache.free_inodes++;
}

// =====================================================================
// ALOKASI BLOCK (caller holds fs_lock)
// =====================================================================
// Alokasi hingga `want` block kontigu (best-effort run terpanjang).
// Return block awal (0 = ENOSPC), *got = jumlah (partial diperbolehkan).
uint64_t blk_alloc_run_nolock(uint32_t want, uint32_t *got) {
    *got = 0;
    if (want == 0) return 0;
    uint64_t total = sb_cache.total_blocks;
    uint64_t run_start = 0, run_len = 0;
    uint64_t best_start = 0; uint32_t best_len = 0;

    for (uint64_t b = data_start; b < total; b++) {
        if (!bm_test(bm_mem, b)) {
            if (run_len == 0) run_start = b;
            run_len++;
            if ((uint32_t)run_len > best_len) {
                best_len = (uint32_t)run_len; best_start = run_start;
            }
            if (run_len >= want) break;             // run pas → selesai
        } else {
            run_len = 0;
        }
    }
    if (best_len == 0) return 0;
    uint64_t start = best_start;
    uint32_t n = (best_len >= want) ? want : best_len;   // partial diperbolehkan

    for (uint64_t b = start; b < start + n; b++) bm_set(bm_mem, b);
    bm_dirty = 1;
    sb_cache.free_blocks -= n;
    *got = n;
    return start;
}

void blk_free_run_nolock(uint64_t start, uint32_t count) {
    for (uint64_t b = start; b < start + count && b < sb_cache.total_blocks; b++) {
        if (bm_test(bm_mem, b)) { bm_clear(bm_mem, b); sb_cache.free_blocks++; }
    }
    bm_dirty = 1;
}
