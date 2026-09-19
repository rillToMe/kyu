// kernel/fs/bcache.c — LRU Block Cache 4KB (Modul 1 KyuzenFS V4).
//
// Penengah antara FS (KyuzenFS V4) dan driver ATA. Satu entry = 1 Block
// 4KB (8 sektor LBA). Write-back: block dirty ditulis saat evict/flush.
//
// LOCKING — disengaja sederhana: SATU spinlock (irqsave) dipegang selama
// seluruh operasi termasuk I/O ATA. Ini aman karena:
//   * ATA PIO murni polling port I/O — tidak tidur, tidak butuh interrupt;
//   * hanya ADA SATU disk — I/O memang serial di perangkat;
//   * lock order tetap satu arah: fs_lock(V4) -> bcache_lock, tidak pernah
//     dibalik, sehingga tidak ada deadlock antar CPU.
// Konsekuensi: I/O disk menahan spinlock ~puluhan mikrodetik-sekian per
// sektor (PIO polling). Untuk OS edukasi single-disk ini diterima; driver
// interrupt-driven nanti tinggal mengganti bagian I/O dengan pin/lepas-lock.
//
// Eviction LRU: doubly-linked list, MRU di depan. Entry ber-refcount > 0
// tidak boleh di-evict; bila SEMUA entry dipin saat butuh slot → NULL.

#include "bcache.h"
#include "ata.h"
#include "heap.h"
#include "string.h"
#include "spinlock.h"

// Driver wrapper 4KB (dibuat di drivers/ata.c — Modul 1).
extern int  ata_read_block4k(uint64_t block_num, void *buf);
extern int  ata_write_block4k(uint64_t block_num, const void *buf);

static struct block_buffer *pool       = NULL;   // BCACHE_N_BLOCKS entry
static struct block_buffer *hash_table[BCACHE_HASH_SIZE];
static struct block_buffer *lru_head   = NULL;   // paling baru dipakai
static struct block_buffer *lru_tail   = NULL;   // kandidat evict
static spinlock_t bcache_lock = SPINLOCK_INIT;

static uint64_t stat_hit  = 0;
static uint64_t stat_miss = 0;

int bcache_init(void) {
    if (pool) return 0;                     // sudah init
    pool = (struct block_buffer*)kmalloc(sizeof(struct block_buffer) * BCACHE_N_BLOCKS);
    if (!pool) return -1;
    memset(pool, 0, sizeof(struct block_buffer) * BCACHE_N_BLOCKS);
    // Semua entry mulai bebas: lru_prev/lru_next NULL (memset) — entry bebas
    // TIDAK berada di list LRU mana pun; alokasi via scan pool di
    // alloc_entry_locked(). Jangan rantai entry bebas lewat lru_next:
    // list LRU harus selalu konsisten (hanya node yang benar-benar
    // ter-cache, terhubung dua arah).
    lru_head = lru_tail = NULL;
    for (int i = 0; i < BCACHE_HASH_SIZE; i++) hash_table[i] = NULL;
    return 0;
}

// --- LRU helpers (caller holds bcache_lock) ---
static void lru_unlink(struct block_buffer *b) {
    if (b->lru_prev) b->lru_prev->lru_next = b->lru_next;
    else             lru_head              = b->lru_next;
    if (b->lru_next) b->lru_next->lru_prev = b->lru_prev;
    else             lru_tail              = b->lru_prev;
    b->lru_prev = b->lru_next = NULL;
}

static void lru_push_front(struct block_buffer *b) {
    // Defensive: node yang masih tertaut (mis. bekas evict yang tidak
    // di-unlink) tidak boleh ditautkan dua kali — itu membuat list
    // bercabang/bersiklus dan traversal jadi infinite loop.
    if (b->lru_prev || b->lru_next || lru_head == b || lru_tail == b)
        lru_unlink(b);
    b->lru_prev = NULL;
    b->lru_next = lru_head;
    if (lru_head) lru_head->lru_prev = b;
    lru_head = b;
    if (!lru_tail) lru_tail = b;
}

static void lru_touch(struct block_buffer *b) {
    if (lru_head == b) return;
    lru_unlink(b);
    lru_push_front(b);
}

// --- Hash map (caller holds bcache_lock) ---
static uint32_t bhash(uint64_t block) { return (uint32_t)(block & (BCACHE_HASH_SIZE - 1)); }

static struct block_buffer* hash_find(uint64_t block) {
    for (struct block_buffer *e = hash_table[bhash(block)]; e; e = e->hash_next)
        if (e->valid && e->block_num == block) return e;
    return NULL;
}

static void hash_insert(struct block_buffer *b) {
    uint32_t h = bhash(b->block_num);
    b->hash_next = hash_table[h];
    hash_table[h] = b;
}

static void hash_remove(struct block_buffer *b) {
    struct block_buffer **pp = &hash_table[bhash(b->block_num)];
    while (*pp) {
        if (*pp == b) { *pp = b->hash_next; b->hash_next = NULL; return; }
        pp = &(*pp)->hash_next;
    }
}

// Cari entry bebas: pool entry yang sudah dikosongkan (evicted / read
// gagal: valid==0, refcount==0) ATAU evict LRU tail yang tidak dipin.
// Guard `!dirty` di slot bebas: entry dirty berisi data yang belum
// sampai ke disk — tidak boleh dipakai diam-diam.
// Caller holds lock.
static struct block_buffer* alloc_entry_locked(void) {
    // 1. Entry pool yang sudah dikosongkan.
    for (int i = 0; i < BCACHE_N_BLOCKS; i++) {
        if (!pool[i].valid && !pool[i].dirty && pool[i].refcount == 0) {
            return &pool[i];
        }
    }
    // 2. Evict LRU tail yang tidak dipin (dirty apa pun wajib write-back).
    for (struct block_buffer *b = lru_tail; b; b = b->lru_prev) {
        if (b->refcount > 0) continue;
        if (b->dirty) {
            // write-back TANPA melepas lock — lihat catatan locking di atas.
            (void)ata_write_block4k(b->block_num, b->data);
            b->dirty = BCACHE_CLEAN;
        }
        hash_remove(b);
        lru_unlink(b);          // WAJIB: entry yang dikosongkan harus lepas
        b->valid = 0;           // dari LRU — kalau tidak, push ulang nanti
        return b;               // menautkannya dua kali (list korup).
    }
    return NULL;   // semua dipin / pool penuh
}

struct block_buffer* bcache_get(uint64_t block_num) {
    uint64_t f = spinlock_lock_irqsave(&bcache_lock);
    struct block_buffer *b = hash_find(block_num);
    if (b) {
        stat_hit++;
        b->refcount++;
        lru_touch(b);
        spinlock_unlock_irqrestore(&bcache_lock, f);
        return b;
    }
    stat_miss++;
    b = alloc_entry_locked();
    if (!b) { spinlock_unlock_irqrestore(&bcache_lock, f); return NULL; }
    b->block_num = block_num;
    b->refcount  = 1;
    b->dirty     = BCACHE_CLEAN;
    // KONTRAK bcache_get: caller WAJIB mengisi data sebelum release
    // (dipakai hanya untuk overwrite penuh / block baru yang isinya
    // ditulis segera). valid=1 sejak sini supaya entry terlihat di hash
    // dan dirty-nya ikut di-flush — bug lama: valid=0 membuat tulisan
    // block baru tidak pernah sampai disk.
    b->valid     = 1;
    hash_insert(b);
    lru_push_front(b);
    spinlock_unlock_irqrestore(&bcache_lock, f);
    return b;
}

struct block_buffer* bcache_read(uint64_t block_num) {
    uint64_t f = spinlock_lock_irqsave(&bcache_lock);
    struct block_buffer *b = hash_find(block_num);
    if (b) {
        stat_hit++;
        b->refcount++;
        lru_touch(b);
        spinlock_unlock_irqrestore(&bcache_lock, f);
        return b;
    }
    stat_miss++;
    b = alloc_entry_locked();
    if (!b) { spinlock_unlock_irqrestore(&bcache_lock, f); return NULL; }
    b->block_num = block_num;
    b->refcount  = 1;
    b->dirty     = BCACHE_CLEAN;
    b->valid     = 0;
    hash_insert(b);
    lru_push_front(b);

    // I/O disk: buffer sudah dipin (refcount 1) — aman dipakai setelah lock
    // dilepas terhadap evictor lain. Tapi lock kita satu-arah dan I/O di
    // luar lock butuh guard terhadap alloc_entry yang menulis dirty entry:
    // kita PERTAHANKAN lock di sini (lihat catatan locking) — sederhana & benar.
    int rc = ata_read_block4k(block_num, b->data);
    if (rc != 0) {
        hash_remove(b);
        b->valid = 0;
        lru_unlink(b);
        b->refcount = 0;
        spinlock_unlock_irqrestore(&bcache_lock, f);
        return NULL;
    }
    b->valid = 1;
    spinlock_unlock_irqrestore(&bcache_lock, f);
    return b;
}

void bcache_mark_dirty(struct block_buffer *b) {
    if (!b) return;
    uint64_t f = spinlock_lock_irqsave(&bcache_lock);
    b->dirty = BCACHE_DIRTY;
    spinlock_unlock_irqrestore(&bcache_lock, f);
}

int bcache_sync_block(struct block_buffer *b) {
    if (!b) return -1;
    uint64_t f = spinlock_lock_irqsave(&bcache_lock);
    int rc = 0;
    if (b->dirty) {
        rc = ata_write_block4k(b->block_num, b->data);
        if (rc == 0) b->dirty = BCACHE_CLEAN;
    }
    spinlock_unlock_irqrestore(&bcache_lock, f);
    return rc;
}

void bcache_release(struct block_buffer *b) {
    if (!b) return;
    uint64_t f = spinlock_lock_irqsave(&bcache_lock);
    if (b->refcount > 0) b->refcount--;
    spinlock_unlock_irqrestore(&bcache_lock, f);
}

void bcache_flush_all(void) {
    uint64_t f = spinlock_lock_irqsave(&bcache_lock);
    for (struct block_buffer *b = lru_head; b; b = b->lru_next) {
        if (b->dirty) {                       // dirty selalu ditulis, valid apa pun
            (void)ata_write_block4k(b->block_num, b->data);
            b->dirty = BCACHE_CLEAN;
        }
    }
    spinlock_unlock_irqrestore(&bcache_lock, f);
}

// Versi jalur panic: tidak boleh menunggu lock. Kalau lock tidak bisa diambil
// sekarang, langsung menyerah (return 0) — pemanggil (panic.c) mencatatnya ke
// serial dan tetap melanjutkan reboot/freeze, bukan menggantung di spinlock.
int bcache_flush_all_try(void) {
    uint64_t f;
    if (!spinlock_try_lock_irqsave(&bcache_lock, &f)) return 0;
    for (struct block_buffer *b = lru_head; b; b = b->lru_next) {
        if (b->dirty) {
            (void)ata_write_block4k(b->block_num, b->data);
            b->dirty = BCACHE_CLEAN;
        }
    }
    spinlock_unlock_irqrestore(&bcache_lock, f);
    return 1;
}

uint64_t bcache_hit_count(void)  { return stat_hit; }
uint64_t bcache_miss_count(void) { return stat_miss; }
