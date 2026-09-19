#ifndef BCACHE_H
#define BCACHE_H

// =====================================================================
// bcache — LRU Block Cache 4KB antara FS (KyuzenFS V4) dan driver disk.
//
// Model:
//   * Satu cache entry = 1 Block 4KB (8 sektor LBA 512B).
//   * bcache_read()  → refcounted struct block_buffer* (data selalu valid).
//   * bcache_get()   → seperti read, tapi TIDAK membaca disk jika miss
//                      (untuk full-block overwrite; caller wajib mengisi).
//   * bcache_mark_dirty() → block ditulis balik saat evict/flush/sync.
//   * bcache_release()    → turunkan refcount; refcount 0 → masuk LRU,
//                      evict (write-back bila dirty) saat cache penuh.
//   * bcache_flush_all()  → tulis SEMUA dirty block (shutdown/sync).
//
// Locking: satu spinlock global bcache_lock melindungi hash map, LRU list,
// refcount, dan dirty state. I/O disk (ata_read/write_block4k) dilakukan
// DI LUAR lock (buffer entry dipin lewat refcount) — pola sama dengan
// TTY di vfs_fd.c: pin → lepas lock → I/O → ambil lock lagi.
//
// Eviction: LRU via doubly-linked list. Entry ber-refcount > 0 tidak
// boleh di-evict. Bila semua entry dipin dan butuh slot → -ENOMEM.
// =====================================================================

#include <stdint.h>
#include <stddef.h>

#define BCACHE_BLOCK_SIZE  4096
#define BCACHE_N_BLOCKS    256          // 256 x 4KB = 1MB cache
#define BCACHE_HASH_SIZE   256          // power of 2; hash = block & 255

// Status entry
#define BCACHE_CLEAN 0
#define BCACHE_DIRTY 1

struct block_buffer {
    uint64_t block_num;              // indeks block 4KB (LBA = block*8)
    uint32_t refcount;               // pemakai aktif (>0 = tidak boleh evict)
    uint8_t  dirty;                  // BCACHE_CLEAN / BCACHE_DIRTY
    uint8_t  valid;                  // 0 = isi belum pernah dibaca dari disk
    uint8_t  data[BCACHE_BLOCK_SIZE];
    struct block_buffer *hash_next;  // chain hash map
    struct block_buffer *lru_prev;   // LRU: prev = lebih baru dipakai
    struct block_buffer *lru_next;   // LRU: next = lebih tua dipakai
};

// Inisialisasi cache (alokasi pool sekali saat boot). 0 = sukses.
int  bcache_init(void);

// Ambil block (baca dari disk bila miss). Refcount +1. NULL = ENOMEM/EIO.
struct block_buffer* bcache_read(uint64_t block_num);

// Ambil slot block tanpa baca disk (miss = buffer kosong, valid=0).
// Untuk overwrite penuh: caller wajib mengisi data sebelum release.
struct block_buffer* bcache_get(uint64_t block_num);

// Tandai block sebagai perlu ditulis balik.
void bcache_mark_dirty(struct block_buffer *b);

// Tulis dirty block ini ke disk SEKARANG (tetap ter-cache sebagai clean).
int  bcache_sync_block(struct block_buffer *b);

// Turunkan refcount (wajib dipanggil per bcache_read/get).
void bcache_release(struct block_buffer *b);

// Tulis seluruh dirty block ke disk (shutdown / sync global).
void bcache_flush_all(void);

// Versi BEST-EFFORT untuk jalur panic: kalau bcache_lock sedang dipegang CPU
// lain (atau oleh CPU yang fault — self-deadlock), TIDAK menunggu dan langsung
// return 0. Return 1 kalau seluruh dirty block benar-benar ditulis.
int  bcache_flush_all_try(void);

// Statistik (diagnostik / taskmgr).
uint64_t bcache_hit_count(void);
uint64_t bcache_miss_count(void);

#endif // BCACHE_H
