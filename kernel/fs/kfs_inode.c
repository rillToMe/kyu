// kernel/fs/kfs_inode.c — KyuzenFS V4: I/O disk inode + inode cache
// (split dari kyuzenfs_v4.c).
//
// Canonical runtime inode hidup di ino_pool (hash by nomor inode).
// vnode->fs_data menunjuk ke &e->ino; vnode memegang satu reference
// (e.ino.refcount). ino_get/ino_put adalah satu-satunya pintu refcount.
// Caller holds fs_lock untuk semua fungsi di file ini.

#include "kfs_internal.h"

// --- Inode cache (canonical runtime inode, hash by ino) ---
static ino_entry_t ino_pool[INO_CACHE_MAX];
static ino_entry_t *ino_hash[INO_HASH_SIZE];

// =====================================================================
// KONVERSI on-disk <-> runtime
// =====================================================================
static int ino_disk_read(uint32_t ino, struct kzfs_inode *di) {
    uint64_t blk = it_block0 + (ino - 1) / KZFS_INODES_PER_BLOCK;
    uint32_t off = (uint32_t)(((ino - 1) % KZFS_INODES_PER_BLOCK) * KZFS_INODE_SIZE);
    struct block_buffer *b = bcache_read(blk);
    if (!b) return KZFS_EIO;
    memcpy(di, b->data + off, sizeof(*di));
    bcache_release(b);
    return KZFS_EOK;
}

static int ino_disk_write(uint32_t ino, const struct kzfs_inode *di) {
    uint64_t blk = it_block0 + (ino - 1) / KZFS_INODES_PER_BLOCK;
    uint32_t off = (uint32_t)(((ino - 1) % KZFS_INODES_PER_BLOCK) * KZFS_INODE_SIZE);
    // Read-modify-write WAJIB (bcache_read, bukan bcache_get): kita hanya
    // menulis slot 128B dari 32 slot dalam block ini. bcache_get memberi
    // entry berisi data basi penghuni slot sebelumnya — saat flush, seluruh
    // 4KB ditulis balik dan slot inode lain ketimpa sampah.
    struct block_buffer *b = bcache_read(blk);
    if (!b) return KZFS_EIO;
    memcpy(b->data + off, di, sizeof(*di));
    bcache_mark_dirty(b);
    bcache_release(b);
    return KZFS_EOK;
}

static void ino_to_disk(const kzfs_v4_inode_mem_t *in, struct kzfs_inode *di) {
    memset(di, 0, sizeof(*di));
    di->mode        = in->mode;
    di->links_count = in->links_count;
    di->size_bytes  = in->size_bytes;
    di->blocks_used = in->blocks_used;
    for (int i = 0; i < KZFS_NUM_DIRECT_EXTENTS; i++) di->direct[i] = in->direct[i];
    di->indirect_extent_block = in->indirect_extent_block;
}

static void ino_from_disk(uint32_t ino, const struct kzfs_inode *di,
                          kzfs_v4_inode_mem_t *out) {
    memset(out, 0, sizeof(*out));
    out->ino          = ino;
    out->mode         = di->mode;
    out->links_count  = di->links_count;
    out->size_bytes   = di->size_bytes;
    out->blocks_used  = di->blocks_used;
    for (int i = 0; i < KZFS_NUM_DIRECT_EXTENTS; i++) out->direct[i] = di->direct[i];
    out->indirect_extent_block = di->indirect_extent_block;
}

// Tulis metadata inode runtime ke disk (tidak menyentuh bitmap/superblock).
int ino_sync_nolock(kzfs_v4_inode_mem_t *in) {
    struct kzfs_inode di;
    ino_to_disk(in, &di);
    return ino_disk_write(in->ino, &di);
}

// =====================================================================
// INODE CACHE — get/put canonical entry. Caller holds fs_lock.
// =====================================================================
uint32_t ino_hash_fn(uint32_t ino) { return ino & (INO_HASH_SIZE - 1); }

ino_entry_t* ino_cache_find(uint32_t ino) {
    for (ino_entry_t *e = ino_hash[ino_hash_fn(ino)]; e; e = e->next)
        if (e->used && !e->dead && e->ino.ino == ino) return e;
    return NULL;
}

void ino_cache_unlink(ino_entry_t *e) {
    ino_entry_t **pp = &ino_hash[ino_hash_fn(e->ino.ino)];
    while (*pp && *pp != e) pp = &(*pp)->next;
    if (*pp) *pp = e->next;
    e->next = NULL;
    e->used = 0;
}

// Ambil slot cache: free slot, atau evict entry refcount==0 (flush dirty).
ino_entry_t* ino_cache_slot(void) {
    ino_entry_t *e = NULL;
    for (int i = 0; i < INO_CACHE_MAX && !e; i++)
        if (!ino_pool[i].used) e = &ino_pool[i];
    // 1) entry dead (inode dihapus, refcount sudah 0) — buang tanpa sync.
    for (int i = 0; i < INO_CACHE_MAX && !e; i++) {
        if (ino_pool[i].used && ino_pool[i].dead && ino_pool[i].ino.refcount == 0) {
            ino_cache_unlink(&ino_pool[i]);
            e = &ino_pool[i];
        }
    }
    // 2) entry hidup refcount 0 — flush dirty dulu bila perlu.
    for (int i = 0; i < INO_CACHE_MAX && !e; i++) {
        if (ino_pool[i].used && ino_pool[i].ino.refcount == 0) {
            if (ino_pool[i].ino.dirty) {
                (void)ino_sync_nolock(&ino_pool[i].ino);
                ino_pool[i].ino.dirty = 0;
            }
            ino_cache_unlink(&ino_pool[i]);
            e = &ino_pool[i];
        }
    }
    // Slot dipakai ulang — flag dead lama tidak boleh terbawa: entry baru
    // yang ber-flag dead akan dilewati sync saat ino_put (metadata hilang).
    if (e) e->dead = 0;
    return e;
}

// Get-or-load canonical inode, refcount +1. Return NULL + *rc = error.
ino_entry_t* ino_get_nolock(uint32_t ino, int *rc) {
    if (ino == 0 || (sb_cache.total_inodes && ino > sb_cache.total_inodes)) {
        if (rc) *rc = KZFS_EINVAL;
        return NULL;
    }
    ino_entry_t *e = ino_cache_find(ino);
    if (e) { e->ino.refcount++; if (rc) *rc = KZFS_EOK; return e; }

    e = ino_cache_slot();
    if (!e) { if (rc) *rc = KZFS_ENOMEM; return NULL; }

    struct kzfs_inode di;
    if (ino_disk_read(ino, &di) != KZFS_EOK) { if (rc) *rc = KZFS_EIO; return NULL; }
    ino_from_disk(ino, &di, &e->ino);
    e->ino.refcount = 1;
    e->dead = 0;
    e->used = 1;
    e->next = ino_hash[ino_hash_fn(ino)];
    ino_hash[ino_hash_fn(ino)] = e;
    if (rc) *rc = KZFS_EOK;
    return e;
}

// Turunkan refcount; refcount 0 + dirty → flush metadata. Entry dead
// (inode dihapus saat masih dipinang) dibuang dari cache saat ref 0.
void ino_put_nolock(ino_entry_t *e) {
    if (!e) return;
    if (e->ino.refcount > 0) e->ino.refcount--;
    if (e->ino.refcount != 0) return;
    if (e->dead) { ino_cache_unlink(e); return; }
    if (e->ino.dirty) {
        (void)ino_sync_nolock(&e->ino);
        e->ino.dirty = 0;
    }
}

// Helper container_of + pemeriksaan dead (dipakai vnode ops yang hanya
// memegang kzfs_v4_inode_mem_t*).
ino_entry_t* ino_entry_of(kzfs_v4_inode_mem_t *in) {
    return (ino_entry_t*)((char*)in - offsetof(ino_entry_t, ino));
}

int ino_is_dead(kzfs_v4_inode_mem_t *in) { return ino_entry_of(in)->dead; }

// Kosongkan seluruh cache (mount/format). Entry yang masih dipinang tidak
// boleh ada saat pemanggilan (dipanggil di awal kfs_init/format).
void ino_cache_reset(void) {
    for (int i = 0; i < INO_HASH_SIZE; i++) ino_hash[i] = NULL;
    for (int i = 0; i < INO_CACHE_MAX; i++) {
        ino_pool[i].used = 0;
        ino_pool[i].dead = 0;
        ino_pool[i].ino.refcount = 0;
        ino_pool[i].next = NULL;
    }
}

// Masukkan entry yang sudah diisi (mis. root saat format) ke hash chain.
// Entry wajib punya e->ino.ino valid; used diset di sini.
void ino_cache_insert_nolock(ino_entry_t *e) {
    e->used = 1;
    e->dead = 0;
    e->next = ino_hash[ino_hash_fn(e->ino.ino)];
    ino_hash[ino_hash_fn(e->ino.ino)] = e;
}
