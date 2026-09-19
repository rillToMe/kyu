// kernel/fs/kfs_dir.c — KyuzenFS V4: operasi direktori (dirent), walk
// path, lifecycle child (create/unlink), dan vnode wrap (split dari
// kyuzenfs_v4.c).
//
// Direktori = file biasa berisi kzfs_dir_entry ber-rec_len variabel.
// Caller holds fs_lock untuk semua fungsi di file ini.

#include "kfs_internal.h"

// =====================================================================
// DIRENT (direktori = file berisi kzfs_dir_entry)
// =====================================================================
int dir_add_entry(kzfs_v4_inode_mem_t *dir, const char *name,
                  uint32_t target_ino, uint8_t ftype) {
    size_t nl = strlen(name);
    if (nl == 0 || nl > KZFS_NAME_MAX) return KZFS_ENAMETOOLONG;
    uint16_t rec_len = (uint16_t)((8 + nl + 3) & ~3u);

    uint8_t *tmp = (uint8_t*)kmalloc(KZFS_BLOCK_SIZE);
    if (!tmp) return KZFS_ENOMEM;

    // 1. Cari slot free yang cukup (inode_num==0 dengan rec_len >= rec).
    for (uint64_t off = 0; off < dir->size_bytes; off += KZFS_BLOCK_SIZE) {
        uint32_t chunk = KZFS_BLOCK_SIZE;
        if (off + chunk > dir->size_bytes) chunk = (uint32_t)(dir->size_bytes - off);
        uint32_t got = 0;
        if (blk_read_file(dir, off, tmp, chunk, &got) != KZFS_EOK || got != chunk) {
            kfree(tmp); return KZFS_EIO;
        }
        uint32_t pos = 0;
        while (pos + 8 <= chunk) {
            uint32_t e_ino; uint16_t e_rec;
            memcpy(&e_ino, tmp + pos, 4);
            memcpy(&e_rec, tmp + pos + 4, 2);
            if (e_rec < KZFS_DIRENT_MIN_REC) { kfree(tmp); return KZFS_EIO; }
            if (e_ino == 0 && e_rec >= rec_len) {
                uint8_t rec[KZFS_DIRENT_MAX_REC];
                memset(rec, 0, rec_len);
                memcpy(rec, &target_ino, 4);
                memcpy(rec + 4, &rec_len, 2);
                rec[6] = (uint8_t)nl;
                rec[7] = ftype;
                memcpy(rec + 8, name, nl);
                uint32_t w = 0;
                int rc = blk_write_file(dir, off + pos, rec, rec_len, &w);
                kfree(tmp);
                return rc;
            }
            pos += e_rec;
        }
    }

    // 2. Tidak ada slot — append di ujung file dir.
    uint8_t rec[KZFS_DIRENT_MAX_REC];
    memset(rec, 0, rec_len);
    memcpy(rec, &target_ino, 4);
    memcpy(rec + 4, &rec_len, 2);
    rec[6] = (uint8_t)nl;
    rec[7] = ftype;
    memcpy(rec + 8, name, nl);
    uint32_t w = 0;
    int rc = blk_write_file(dir, dir->size_bytes, rec, rec_len, &w);
    kfree(tmp);
    return rc;
}

int dir_lookup_entry(kzfs_v4_inode_mem_t *dir, const char *name,
                     uint32_t *out_ino, uint8_t *out_ftype) {
    size_t nl = strlen(name);
    if (nl > KZFS_NAME_MAX) return KZFS_ENAMETOOLONG;
    uint8_t *tmp = (uint8_t*)kmalloc(KZFS_BLOCK_SIZE);
    if (!tmp) return KZFS_ENOMEM;
    int result = KZFS_ENOENT;

    for (uint64_t off = 0; off < dir->size_bytes; off += KZFS_BLOCK_SIZE) {
        uint32_t chunk = KZFS_BLOCK_SIZE;
        if (off + chunk > dir->size_bytes) chunk = (uint32_t)(dir->size_bytes - off);
        uint32_t got = 0;
        if (blk_read_file(dir, off, tmp, chunk, &got) != KZFS_EOK) { result = KZFS_EIO; break; }
        uint32_t pos = 0;
        while (pos + 8 <= chunk) {
            uint32_t e_ino; uint16_t e_rec; uint8_t e_nlen, e_ftype;
            memcpy(&e_ino, tmp + pos, 4);
            memcpy(&e_rec, tmp + pos + 4, 2);
            e_nlen  = tmp[pos + 6];
            e_ftype = tmp[pos + 7];
            if (e_rec < KZFS_DIRENT_MIN_REC) { result = KZFS_EIO; break; }
            if (e_ino != 0 && e_nlen == nl && memcmp(tmp + pos + 8, name, nl) == 0) {
                if (out_ino)   *out_ino   = e_ino;
                if (out_ftype) *out_ftype = e_ftype;
                result = KZFS_EOK;
                break;
            }
            pos += e_rec;
        }
        if (result != KZFS_ENOENT) break;
    }
    kfree(tmp);
    return result;
}

// Hapus entri: inode_num = 0 (slot free), rec_len slot dipertahankan.
int dir_remove_entry(kzfs_v4_inode_mem_t *dir, const char *name) {
    size_t nl = strlen(name);
    if (nl > KZFS_NAME_MAX) return KZFS_ENAMETOOLONG;
    uint8_t *tmp = (uint8_t*)kmalloc(KZFS_BLOCK_SIZE);
    if (!tmp) return KZFS_ENOMEM;
    int result = KZFS_ENOENT;

    for (uint64_t off = 0; off < dir->size_bytes; off += KZFS_BLOCK_SIZE) {
        uint32_t chunk = KZFS_BLOCK_SIZE;
        if (off + chunk > dir->size_bytes) chunk = (uint32_t)(dir->size_bytes - off);
        uint32_t got = 0;
        if (blk_read_file(dir, off, tmp, chunk, &got) != KZFS_EOK) { result = KZFS_EIO; break; }
        uint32_t pos = 0;
        while (pos + 8 <= chunk) {
            uint32_t e_ino; uint16_t e_rec; uint8_t e_nlen;
            memcpy(&e_ino, tmp + pos, 4);
            memcpy(&e_rec, tmp + pos + 4, 2);
            e_nlen = tmp[pos + 6];
            if (e_rec < KZFS_DIRENT_MIN_REC) { result = KZFS_EIO; break; }
            if (e_ino != 0 && e_nlen == nl && memcmp(tmp + pos + 8, name, nl) == 0) {
                uint16_t freelen = e_rec;                 // inode=0, rec_len asli
                uint32_t zero_ino = 0;
                uint32_t w = 0;
                memcpy(tmp + pos, &zero_ino, 4);
                memcpy(tmp + pos + 4, &freelen, 2);
                memset(tmp + pos + 6, 0, e_rec - 6);
                int rc = blk_write_file(dir, off + pos, tmp + pos, e_rec, &w);
                result = rc;
                break;
            }
            pos += e_rec;
        }
        if (result != KZFS_ENOENT) break;
    }
    kfree(tmp);
    return result;
}

// Enumerasi entri ke-index (melewati slot free). Return 0/ENOENT.
int dir_read_index(kzfs_v4_inode_mem_t *dir, uint32_t index,
                   char *name_out, uint32_t cap, uint8_t *type_out) {
    uint8_t *tmp = (uint8_t*)kmalloc(KZFS_BLOCK_SIZE);
    if (!tmp) return KZFS_ENOMEM;
    uint32_t seen = 0;
    int result = KZFS_ENOENT;

    for (uint64_t off = 0; off < dir->size_bytes; off += KZFS_BLOCK_SIZE) {
        uint32_t chunk = KZFS_BLOCK_SIZE;
        if (off + chunk > dir->size_bytes) chunk = (uint32_t)(dir->size_bytes - off);
        uint32_t got = 0;
        if (blk_read_file(dir, off, tmp, chunk, &got) != KZFS_EOK) { result = KZFS_EIO; break; }
        uint32_t pos = 0;
        while (pos + 8 <= chunk) {
            uint32_t e_ino; uint16_t e_rec; uint8_t e_nlen, e_ftype;
            memcpy(&e_ino, tmp + pos, 4);
            memcpy(&e_rec, tmp + pos + 4, 2);
            e_nlen  = tmp[pos + 6];
            e_ftype = tmp[pos + 7];
            if (e_rec < KZFS_DIRENT_MIN_REC) { result = KZFS_EIO; break; }
            if (e_ino != 0) {
                if (seen == index) {
                    if (e_nlen + 1 > cap) { result = KZFS_ENAMETOOLONG; break; }
                    memcpy(name_out, tmp + pos + 8, e_nlen);
                    name_out[e_nlen] = '\0';
                    if (type_out) *type_out = e_ftype;
                    result = KZFS_EOK;
                    break;
                }
                seen++;
            }
            pos += e_rec;
        }
        if (result != KZFS_ENOENT) break;
    }
    kfree(tmp);
    return result;
}

int dir_is_dot(const char *nm) {
    return nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'));
}

// =====================================================================
// VNODE WRAP — vnode memegang 1 reference ke canonical inode cache.
// Caller holds fs_lock.
// =====================================================================
int vnode_wrap_locked(ino_entry_t *e, struct vnode **out) {
    struct vnode *v = (struct vnode*)kmalloc(sizeof(struct vnode));
    if (!v) return KZFS_ENOMEM;
    v->type      = (e->ino.mode == KZFS_INODE_FLAG_DIR) ? V_DIR : V_REG;
    v->refcount  = 1;
    v->size      = e->ino.size_bytes;
    v->inode_num = e->ino.ino;
    v->ops       = &kzfs_v4_vnode_ops;
    v->fs_data   = &e->ino;            // canonical, hidup selama refcount>0
    *out = v;
    return KZFS_EOK;
}

// =====================================================================
// PATH HELPERS (semua return vnode ber-refcount, atau NULL)
// =====================================================================
static int path_is_root(const char *path) {
    while (*path == '/') path++;
    return *path == '\0';
}

// Walk path dari root (dipakai shim API lama yang menerima path string).
struct vnode* kfs_walk(const char *path) {
    if (!fs_mounted) return NULL;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    ino_entry_t *re = ino_get_nolock(root_ino, NULL);
    if (!re) { spinlock_unlock_irqrestore(&fs_lock, flags); return NULL; }
    struct vnode *cur = NULL;
    if (vnode_wrap_locked(re, &cur) != KZFS_EOK) {
        ino_put_nolock(re);
        spinlock_unlock_irqrestore(&fs_lock, flags);
        return NULL;
    }
    // Referensi dari ino_get_nolock KINI milik vnode — tidak di-put di sini.
    spinlock_unlock_irqrestore(&fs_lock, flags);

    if (path_is_root(path)) return cur;

    char comp[KZFS_NAME_MAX + 2];
    size_t i = 0;
    while (path[i]) {
        while (path[i] == '/') i++;
        if (!path[i]) break;
        size_t s = i;
        while (path[i] && path[i] != '/') i++;
        size_t len = i - s;
        if (len > KZFS_NAME_MAX) { cur->ops->release(cur); return NULL; }
        memcpy(comp, path + s, len);
        comp[len] = '\0';

        struct vnode *next = NULL;
        if (cur->ops->lookup(cur, comp, &next) != KZFS_EOK || !next) {
            cur->ops->release(cur);
            return NULL;
        }
        cur->ops->release(cur);
        cur = next;
    }
    return cur;
}

// Pecah path jadi (parent_vnode, nama terakhir). Parent ber-refcount.
struct vnode* kfs_walk_parent(const char *path, const char **out_name) {
    const char *name = path;
    for (const char *p = path; *p; p++) if (*p == '/') name = p + 1;
    if (*name == '\0') return NULL;             // path berakhir '/'
    *out_name = name;
    size_t plen = (size_t)(name - path);
    if (plen == 0) return kfs_walk("/");        // tanpa '/' → root
    char pp[512];
    if (plen >= sizeof(pp)) return NULL;
    memcpy(pp, path, plen - 1);                 // buang slash terakhir
    pp[plen - 1] = '\0';
    return kfs_walk(pp);
}

// =====================================================================
// INODE LIFECYCLE (create/free) — caller holds fs_lock
// =====================================================================
// Isi block pertama folder baru dengan "." dan "..".
int dir_init_dots(kzfs_v4_inode_mem_t *dir, uint32_t parent_ino) {
    uint8_t *blk = (uint8_t*)kmalloc(KZFS_BLOCK_SIZE);
    if (!blk) return KZFS_ENOMEM;
    memset(blk, 0, KZFS_BLOCK_SIZE);

    struct kzfs_dir_entry dot;
    memset(&dot, 0, sizeof(dot));
    dot.inode_num = dir->ino;
    dot.rec_len   = (uint16_t)((8 + 1 + 3) & ~3u);
    dot.name_len  = 1;
    dot.file_type = KZFS_INODE_FLAG_DIR;
    dot.name[0]   = '.';
    memcpy(blk, &dot, 8 + 1);

    struct kzfs_dir_entry dotdot = dot;
    dotdot.inode_num = parent_ino;
    dotdot.rec_len   = (uint16_t)(KZFS_BLOCK_SIZE - dot.rec_len);   // sisa block
    dotdot.name_len  = 2;
    dotdot.name[1]   = '.';
    memcpy(blk + dot.rec_len, &dotdot, 8 + 2);

    uint32_t w = 0;
    int rc = blk_write_file(dir, 0, blk, KZFS_BLOCK_SIZE, &w);
    kfree(blk);
    if (rc == KZFS_EOK) { dir->links_count = 2; dir->dirty = 1; }
    return rc;
}

// Buat child (file/dir) di parent. Saat sukses, *out_entry DIPINANG
// (refcount 1) — caller wajib melepaskannya (vnode / ino_put_nolock).
int create_child(kzfs_v4_inode_mem_t *parent, const char *name,
                 uint16_t mode, ino_entry_t **out_entry) {
    if (parent->mode != KZFS_INODE_FLAG_DIR) return KZFS_ENOTDIR;
    size_t nl = strlen(name);
    if (nl == 0) return KZFS_EINVAL;
    if (nl > KZFS_NAME_MAX) return KZFS_ENAMETOOLONG;

    uint32_t dummy; uint8_t ft;
    if (dir_lookup_entry(parent, name, &dummy, &ft) == KZFS_EOK)
        return KZFS_EEXIST;

    int32_t newino = ino_alloc_nolock();
    if (newino < 0) return KZFS_ENOSPC;

    ino_entry_t *ce = ino_cache_slot();
    if (!ce) { ino_free_nolock((uint32_t)newino); return KZFS_ENOMEM; }
    memset(&ce->ino, 0, sizeof(ce->ino));
    ce->ino.ino         = (uint32_t)newino;
    ce->ino.mode        = mode;
    ce->ino.links_count = 1;
    ce->ino.dirty       = 1;
    ce->ino.refcount    = 1;                    // dipinang caller (vnode)
    ino_cache_insert_nolock(ce);                // used=1 + masuk hash chain

    int rc = KZFS_EOK;
    if (mode == KZFS_INODE_FLAG_DIR) {
        rc = dir_init_dots(&ce->ino, parent->ino);
        if (rc == KZFS_EOK) parent->links_count++;   // ".." child → parent
    }
    if (rc == KZFS_EOK)
        rc = dir_add_entry(parent, name, (uint32_t)newino,
                           (uint8_t)((mode == KZFS_INODE_FLAG_DIR) ? KZFS_INODE_FLAG_DIR
                                                                   : KZFS_INODE_FLAG_FILE));
    if (rc != KZFS_EOK) {
        if (mode == KZFS_INODE_FLAG_DIR) parent->links_count--;
        inode_free_all_extents(&ce->ino);
        ce->ino.dirty = 0;
        ce->ino.refcount = 0;                    // lepas pin caller
        ino_free_nolock((uint32_t)newino);       // dead + kosongkan bitmap
        parent->dirty = 1;
        return rc;
    }
    (void)ino_sync_nolock(&ce->ino);
    ce->ino.dirty = 0;
    parent->dirty = 1;
    (void)ino_sync_nolock(parent);
    *out_entry = ce;
    return KZFS_EOK;
}
