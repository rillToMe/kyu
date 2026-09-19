// kernel/fs/kfs_vnode.c — KyuzenFS V4: vnode operations (split dari
// kyuzenfs_v4.c).
//
// Implementasi struct vnode_ops yang dipakai vfs_fd.c. Semua operasi
// menerima struct vnode* dan bekerja pada canonical inode cache lewat
// vnode->fs_data. Kontrak refcount: vnode memegang SATU reference inode;
// release() melepaskannya. Locking: fs_lock per operasi.

#include "kfs_internal.h"

static int v4_open(struct vnode *n, int flags) { (void)n; (void)flags; return KZFS_EOK; }

static int v4_read(struct vnode *n, uint64_t offset, void *buf,
                   uint64_t count, uint64_t *bytes_read) {
    if (!n || !n->fs_data || !buf) return KZFS_EINVAL;
    if (n->type == V_DIR) return KZFS_EISDIR;
    uint64_t got_total = 0;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    kzfs_v4_inode_mem_t *in = (kzfs_v4_inode_mem_t*)n->fs_data;
    int rc = KZFS_EOK;
    const uint64_t CHUNK = 1024 * 1024;
    // Loop chunking: kondisi pada got_total (bukan count — count tidak
    // pernah berubah di dalam loop; bug lama bikin baca tak terbatas).
    while (got_total < count && rc == KZFS_EOK) {
        uint64_t remain = count - got_total;
        uint32_t piece = (remain > CHUNK) ? (uint32_t)CHUNK : (uint32_t)remain;
        uint32_t got = 0;
        rc = blk_read_file(in, offset + got_total, (uint8_t*)buf + got_total, piece, &got);
        got_total += got;
        if (got < piece) break;                  // EOF
    }
    spinlock_unlock_irqrestore(&fs_lock, flags);
    if (rc == KZFS_EOK && bytes_read) *bytes_read = got_total;
    return rc;
}

static int v4_write(struct vnode *n, uint64_t offset, const void *buf,
                    uint64_t count, uint64_t *bytes_written) {
    if (!n || !n->fs_data || !buf) return KZFS_EINVAL;
    if (n->type == V_DIR) return KZFS_EISDIR;
    uint64_t wtotal = 0;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    kzfs_v4_inode_mem_t *in = (kzfs_v4_inode_mem_t*)n->fs_data;
    int rc = KZFS_EOK;
    const uint64_t CHUNK = 1024 * 1024;
    // Loop chunking: kondisi pada wtotal (bukan count — bug lama bikin
    // tulis tak terbatas sampai disk penuh / ENOSPC).
    while (wtotal < count && rc == KZFS_EOK) {
        uint64_t remain = count - wtotal;
        uint32_t piece = (remain > CHUNK) ? (uint32_t)CHUNK : (uint32_t)remain;
        uint32_t w = 0;
        rc = blk_write_file(in, offset + wtotal, (const uint8_t*)buf + wtotal, piece, &w);
        wtotal += w;
        if (w < piece) break;                    // ENOSPC / tak ada progres
    }
    if (rc == KZFS_EOK && wtotal > 0) {
        (void)ino_sync_nolock(in);
        (void)bm_flush();
        (void)sb_save();
    }
    n->size = in->size_bytes;
    spinlock_unlock_irqrestore(&fs_lock, flags);
    if (rc == KZFS_EOK && bytes_written) *bytes_written = wtotal;
    return rc;
}

static int v4_truncate(struct vnode *n, uint64_t new_size) {
    if (!n || !n->fs_data) return KZFS_EINVAL;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    kzfs_v4_inode_mem_t *in = (kzfs_v4_inode_mem_t*)n->fs_data;
    int rc = truncate_nolock(in, new_size);
    if (rc == KZFS_EOK) {
        (void)ino_sync_nolock(in);
        (void)bm_flush();
        (void)sb_save();
        n->size = in->size_bytes;
    }
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return rc;
}

static int v4_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    if (!dir || !out || dir->type != V_DIR) return KZFS_ENOTDIR;
    if (!name || !*name || dir_is_dot(name)) return KZFS_EINVAL;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    kzfs_v4_inode_mem_t *d = (kzfs_v4_inode_mem_t*)dir->fs_data;
    uint32_t ino; uint8_t ft;
    int rc = dir_lookup_entry(d, name, &ino, &ft);
    if (rc == KZFS_EOK) {
        // ino_get_nolock menaikkan ref ke 1; ref itu DIPINDAH ke vnode
        // (vnode_wrap_locked TIDAK menambah lagi) — ino_put TIDAK dipanggil.
        ino_entry_t *e = ino_get_nolock(ino, &rc);
        if (e) rc = vnode_wrap_locked(e, out);
    }
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return rc;
}

static int v4_create(struct vnode *dir, const char *name, uint16_t mode,
                     struct vnode **out) {
    if (!dir || dir->type != V_DIR) return KZFS_ENOTDIR;
    (void)mode;                                  // V4: file selalu FLAG_FILE
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    kzfs_v4_inode_mem_t *d = (kzfs_v4_inode_mem_t*)dir->fs_data;
    ino_entry_t *ce = NULL;
    // create_child meminangkan entry (refcount 1) — referensi pindah ke vnode.
    int rc = create_child(d, name, KZFS_INODE_FLAG_FILE, &ce);
    if (rc == KZFS_EOK) rc = vnode_wrap_locked(ce, out);
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return rc;
}

static int v4_mkdir(struct vnode *dir, const char *name) {
    if (!dir || dir->type != V_DIR) return KZFS_ENOTDIR;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    kzfs_v4_inode_mem_t *d = (kzfs_v4_inode_mem_t*)dir->fs_data;
    ino_entry_t *ce = NULL;
    int rc = create_child(d, name, KZFS_INODE_FLAG_DIR, &ce);
    if (rc == KZFS_EOK) {
        // Tidak ada vnode yang dibuat — lepaskan ref pinangan create_child.
        ino_put_nolock(ce);
    }
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return rc;
}

static int v4_unlink(struct vnode *dir, const char *name) {
    if (!dir || dir->type != V_DIR) return KZFS_ENOTDIR;
    if (!name || !*name || dir_is_dot(name)) return KZFS_EINVAL;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    kzfs_v4_inode_mem_t *d = (kzfs_v4_inode_mem_t*)dir->fs_data;
    uint32_t ino; uint8_t ft;
    int rc = dir_lookup_entry(d, name, &ino, &ft);
    if (rc != KZFS_EOK) { spinlock_unlock_irqrestore(&fs_lock, flags); return rc; }
    if (ino == root_ino) { spinlock_unlock_irqrestore(&fs_lock, flags); return KZFS_EINVAL; }

    ino_entry_t *ve = ino_get_nolock(ino, &rc);
    if (!ve) { spinlock_unlock_irqrestore(&fs_lock, flags); return rc; }

    // Folder hanya boleh dihapus bila kosong (hanya "." dan "..").
    if (ve->ino.mode == KZFS_INODE_FLAG_DIR) {
        char nm[KZFS_NAME_MAX + 2]; uint8_t ty;
        uint32_t live = 0;
        for (uint32_t i = 0; ; i++) {
            if (dir_read_index(&ve->ino, i, nm, sizeof(nm), &ty) != KZFS_EOK) break;
            if (!dir_is_dot(nm)) live++;
        }
        if (live != 0) {
            ino_put_nolock(ve);
            spinlock_unlock_irqrestore(&fs_lock, flags);
            return KZFS_EEXIST;
        }
        d->links_count--;                        // ".." victim menghilang
    }

    rc = dir_remove_entry(d, name);
    if (rc == KZFS_EOK) {
        inode_free_all_extents(&ve->ino);
        ve->ino.links_count = 0;
        ve->ino.dirty = 0;                       // inode mati — jangan sync
        ino_free_nolock(ve->ino.ino);            // tandai dead + kosongkan bitmap
        (void)ino_sync_nolock(d);
        (void)bm_flush();
        (void)ibm_flush();
        (void)sb_save();
    }
    ino_put_nolock(ve);
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return rc;
}

static int v4_readdir(struct vnode *dir, uint32_t index,
                      char *name_out, uint32_t name_cap, uint8_t *type_out) {
    if (!dir || dir->type != V_DIR) return KZFS_ENOTDIR;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    kzfs_v4_inode_mem_t *d = (kzfs_v4_inode_mem_t*)dir->fs_data;
    int rc = dir_read_index(d, index, name_out, name_cap, type_out);
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return rc;
}

static int v4_sync(struct vnode *n) {
    if (!n || !n->fs_data) return KZFS_EINVAL;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    kzfs_v4_inode_mem_t *in = (kzfs_v4_inode_mem_t*)n->fs_data;
    int rc = ino_sync_nolock(in);
    if (rc == KZFS_EOK) {
        (void)bm_flush();
        (void)ibm_flush();
        (void)sb_save();
    }
    spinlock_unlock_irqrestore(&fs_lock, flags);
    bcache_flush_all();
    return rc;
}

static void v4_release(struct vnode *n) {
    if (!n) return;
    if (n->refcount > 1) { n->refcount--; return; }
    if (n->fs_data) {
        uint64_t flags = spinlock_lock_irqsave(&fs_lock);
        ino_put_nolock(ino_entry_of((kzfs_v4_inode_mem_t*)n->fs_data));
        spinlock_unlock_irqrestore(&fs_lock, flags);
    }
    kfree(n);
}

struct vnode_ops kzfs_v4_vnode_ops = {
    .open     = v4_open,
    .read     = v4_read,
    .write    = v4_write,
    .lookup   = v4_lookup,
    .create   = v4_create,
    .truncate = v4_truncate,
    .mkdir    = v4_mkdir,
    .unlink   = v4_unlink,
    .readdir  = v4_readdir,
    .sync     = v4_sync,
    .release  = v4_release,
};
