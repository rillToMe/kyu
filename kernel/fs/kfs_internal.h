#ifndef KFS_INTERNAL_H
#define KFS_INTERNAL_H

// =====================================================================
// kernel/fs/kfs_internal.h — Kontrak INTERNAL antar-modul KyuzenFS V4.
//
// File ini BUKAN API publik: hanya dipakai oleh file .c di kernel/fs/.
// Caller luar tetap lewat include/kyuzenfs.h (shim kfs_*) dan
// include/vnode.h (vnode ops).
//
// Split dari kyuzenfs_v4.c (monolitik ~1580 baris) menjadi:
//   kfs_super.c   — state global, superblock, layout, mount, format, sync
//   kfs_balloc.c  — bitmap helper + alokasi/bebas inode & block (run)
//   kfs_inode.c   — inode cache (canonical runtime inode) + I/O disk inode
//   kfs_extent.c  — extent engine + block-level file read/write + truncate
//   kfs_dir.c     — dirent, walk path, create/unlink child, vnode wrap
//   kfs_vnode.c   — vnode operations (struct kzfs_v4_vnode_ops)
//   kfs_shim.c    — API kompatibilitas lama kfs_* untuk caller V3
//
// Desain & kepemilikan inode (penting, pernah salah):
//   * Canonical runtime inode hidup di ino_cache (ino_pool). vnode->fs_data
//     MENUNJUK ke entry cache (bukan copy stack), dan vnode memegang SATU
//     reference (entry.ino.refcount). Refcount 0 → entry boleh di-evict
//     (metadata dirty diflush dulu).
//   * Locking: satu spinlock global fs_lock (irqsave) untuk semua metadata;
//     bcache punya lock sendiri — order fs_lock -> bcache_lock, tidak
//     dibalik. Semua fungsi ber-suffix _nolock WAJIB dipanggil sambil
//     memegang fs_lock.
//   * Read/write file STREAMING per-block via bcache — file tidak pernah
//     dibuffer utuh di RAM. Write IN-PLACE pada block fisik.
//
// Lapisan:
//   [vfs_fd.c] --vnode_ops--> [kfs_*.c] --bcache--> [ata 4KB]
// =====================================================================

#include "kyuzenfs_v4.h"
#include "kyuzenfs.h"      // kontrak publik (resolusi path, rename, stat)
#include "vnode.h"
#include "bcache.h"
#include "ata.h"
#include "heap.h"
#include "string.h"
#include "spinlock.h"
#include <stddef.h>

extern void     kprint(const char* s);
extern uint32_t ata_get_total_sectors(void);

// ---------------------------------------------------------------------
// STATE GLOBAL FS (didefinisikan di kfs_super.c)
// ---------------------------------------------------------------------
extern struct kzfs_superblock sb_cache;   // superblock di RAM
extern int      fs_mounted;               // 1 bila disk V4 termount

extern uint8_t *bm_mem;                   // Block Bitmap penuh di RAM
extern uint32_t bm_blocks;
extern int      bm_dirty;

extern uint8_t *ibm_mem;                  // Inode Bitmap penuh di RAM
extern uint32_t ibm_blocks;
extern int      ibm_dirty;

extern uint64_t it_block0;                // block pertama Inode Table
extern uint32_t root_ino;                 // root dir inode (1)
extern uint64_t data_start;               // block pertama area data

extern spinlock_t fs_lock;

// ---------------------------------------------------------------------
// INODE CACHE — tipe & batas (implementasi di kfs_inode.c)
// ---------------------------------------------------------------------
#define INO_HASH_SIZE 128
#define INO_CACHE_MAX 64

typedef struct ino_entry {
    kzfs_v4_inode_mem_t ino;     // canonical copy; refcount di dalamnya
    int used;
    int dead;                    // inode sudah dihapus tapi masih dipinang
                                 // (vnode terbuka) — jangan dipakai ulang/disync
    struct ino_entry *next;      // hash chain
} ino_entry_t;

// ---------------------------------------------------------------------
// EXTENT ENGINE — batas (implementasi di kfs_extent.c)
// ---------------------------------------------------------------------
#define KZFS_EXTS_PER_IND_BLOCK (KZFS_BLOCK_SIZE / 12)   // 341
#define KZFS_MAX_EXTENTS (KZFS_EXTS_PER_IND_BLOCK + KZFS_NUM_DIRECT_EXTENTS)

// ---------------------------------------------------------------------
// kfs_super.c — superblock, layout, mount, format, sync
// ---------------------------------------------------------------------
int  sb_load(void);                            // baca+validasi superblock
int  sb_save(void);                            // tulis superblock (dirty)
int  mount_load_bitmaps(void);                 // muat bitmap dari disk
int  bm_flush(void);                           // bitmap block → disk
int  ibm_flush(void);                          // bitmap inode → disk
void kfs_init(void);                           // mount / format otomatis
void kfs_format(void);                         // format eksplisit (syscall 5)
int  kfs_v4_is_mounted(void);
void kfs_v4_get_stats(uint64_t *total_blocks, uint64_t *free_blocks,
                      uint32_t *total_inodes, uint32_t *free_inodes);
void kfs_sync_all(void);                       // timer berkala + shutdown
void kfs_v4_cache_stats(uint64_t *hit, uint64_t *miss);

// ---------------------------------------------------------------------
// kfs_balloc.c — bitmap + alokasi inode & block (caller holds fs_lock)
// ---------------------------------------------------------------------
int      bm_test(const uint8_t *m, uint64_t bit);
void     bm_set(uint8_t *m, uint64_t bit);
void     bm_clear(uint8_t *m, uint64_t bit);
int32_t  ino_alloc_nolock(void);               // return nomor inode, -1 ENOSPC
void     ino_free_nolock(uint32_t ino);        // bitmap kosong + tandai dead
uint64_t blk_alloc_run_nolock(uint32_t want, uint32_t *got);   // 0 = ENOSPC
void     blk_free_run_nolock(uint64_t start, uint32_t count);

// ---------------------------------------------------------------------
// kfs_inode.c — inode cache & I/O disk inode (caller holds fs_lock)
// ---------------------------------------------------------------------
uint32_t     ino_hash_fn(uint32_t ino);
ino_entry_t* ino_cache_find(uint32_t ino);     // lewati entry dead
void         ino_cache_unlink(ino_entry_t *e);
ino_entry_t* ino_cache_slot(void);             // free slot / evict ref 0
ino_entry_t* ino_get_nolock(uint32_t ino, int *rc);   // get-or-load, ref +1
void         ino_put_nolock(ino_entry_t *e);   // ref -1; dirty → sync
ino_entry_t* ino_entry_of(kzfs_v4_inode_mem_t *in);   // container_of
int          ino_is_dead(kzfs_v4_inode_mem_t *in);
void         ino_cache_reset(void);            // kosongkan pool+hash (mount/format)
void         ino_cache_insert_nolock(ino_entry_t *e);  // daftarkan entry terisi ke hash
int          ino_sync_nolock(kzfs_v4_inode_mem_t *in);

// ---------------------------------------------------------------------
// kfs_extent.c — extent engine + block-level file I/O (caller holds lock)
// ---------------------------------------------------------------------
int      ext_get_nolock(kzfs_v4_inode_mem_t *in, uint32_t n,
                        struct kzfs_extent *out);
int      ext_set_nolock(kzfs_v4_inode_mem_t *in, uint32_t n,
                        const struct kzfs_extent *e);
uint32_t kzfs_get_phys_block(kzfs_v4_inode_mem_t *inode, uint64_t logical_block,
                             uint64_t *phys_out);      // 1 = mapped, 0 = lubang
int      ext_append_nolock(kzfs_v4_inode_mem_t *in, uint64_t start, uint32_t count);
int      grow_to_nolock(kzfs_v4_inode_mem_t *in, uint64_t logical_block);
int      blk_read_file(kzfs_v4_inode_mem_t *in, uint64_t file_off,
                       void *buf, uint32_t count, uint32_t *nread);
int      blk_write_file(kzfs_v4_inode_mem_t *in, uint64_t file_off,
                        const void *buf, uint32_t count, uint32_t *nwritten);
int      truncate_nolock(kzfs_v4_inode_mem_t *in, uint64_t new_size);
void     inode_free_all_extents(kzfs_v4_inode_mem_t *in);

// ---------------------------------------------------------------------
// kfs_dir.c — dirent, path walk, lifecycle child (kecuali catatan lock)
// ---------------------------------------------------------------------
int            dir_add_entry(kzfs_v4_inode_mem_t *dir, const char *name,
                             uint32_t target_ino, uint8_t ftype);
int            dir_lookup_entry(kzfs_v4_inode_mem_t *dir, const char *name,
                                uint32_t *out_ino, uint8_t *out_ftype);
int            dir_remove_entry(kzfs_v4_inode_mem_t *dir, const char *name);
int            dir_read_index(kzfs_v4_inode_mem_t *dir, uint32_t index,
                              char *name_out, uint32_t cap, uint8_t *type_out);
int            dir_is_dot(const char *nm);           // "." atau ".."
int            dir_init_dots(kzfs_v4_inode_mem_t *dir, uint32_t parent_ino);
// Tulis ulang entri ".." block-pertama direktori (dipakai rename folder).
int            dir_set_dotdot(kzfs_v4_inode_mem_t *dir, uint32_t parent_ino);
int            create_child(kzfs_v4_inode_mem_t *parent, const char *name,
                            uint16_t mode, ino_entry_t **out_entry);
int            vnode_wrap_locked(ino_entry_t *e, struct vnode **out);
// Batas kedalaman path (dipakai resolver + cek subtree rename).
#define KZFS_PATH_MAX_DEPTH 32

// Tabel operasi vnode KyuzenFS V4 (didefinisikan di kfs_vnode.c).
extern struct vnode_ops kzfs_v4_vnode_ops;

#endif // KFS_INTERNAL_H
