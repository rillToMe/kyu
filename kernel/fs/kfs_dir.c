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
            if (e_rec < KZFS_DIRENT_MIN_REC || e_rec > chunk - pos) {
                kfree(tmp); return KZFS_EIO;               // rusak / lintas block
            }
            if (e_ino == 0 && e_rec >= rec_len) {
                // Slot free dipakai ulang. Kalau slot LEBIH BESAR dari yang
                // dibutuhkan, sisanya WAJIB jadi record free sendiri: scan
                // berikutnya melompat lewat rec_len record ini, jadi sisa tanpa
                // header terbaca sebagai rec_len 0 = record rusak dan semua
                // entri sesudahnya hilang. Bila sisanya < rekor minimal, ambil
                // SELURUH slot (buang < 12 byte) supaya rantai tetap utuh.
                // Ini bukan kasus langka: rename = add diikuti remove, sehingga
                // slot free besar di tengah direktori itu normal.
                uint16_t use = rec_len;
                if ((uint32_t)e_rec - rec_len < KZFS_DIRENT_MIN_REC) use = e_rec;

                uint8_t *rec = (uint8_t*)kmalloc(e_rec);
                if (!rec) { kfree(tmp); return KZFS_ENOMEM; }
                memset(rec, 0, e_rec);
                memcpy(rec, &target_ino, 4);
                memcpy(rec + 4, &use, 2);
                rec[6] = (uint8_t)nl;
                rec[7] = ftype;
                memcpy(rec + 8, name, nl);
                if (use < e_rec) {                         // sisa → record free utuh
                    uint32_t zero_ino = 0;
                    uint16_t rest = (uint16_t)(e_rec - use);
                    memcpy(rec + use, &zero_ino, 4);
                    memcpy(rec + use + 4, &rest, 2);
                }
                uint32_t w = 0;
                int rc = blk_write_file(dir, off + pos, rec, e_rec, &w);
                kfree(rec);
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
// PATH HELPERS (semua return vnode ber-refcount)
// =====================================================================
// Resolusi path: lihat kontrak lengkap di include/kyuzenfs.h (kfs_walk_path).
//
// Implementasi memakai STACK ancestor (bukan entri ".." on-disk) supaya
// "/a/b/../c" deterministik: ".." = satu level naik dari posisi sekarang,
// dan di root tetap root. Entri ".." on-disk tetap dipelihara (dir_init_dots /
// dir_set_dotdot) agar struktur di disk tidak berbohong, tapi resolver tidak
// bergantung padanya.
int kfs_walk_path(const char *path, struct vnode **out) {
    if (out) *out = NULL;
    if (!path || !out) return KZFS_EINVAL;
    if (!fs_mounted) return KZFS_EIO;

    struct vnode *stack[KZFS_PATH_MAX_DEPTH + 1];   // slot 0 = root
    uint32_t depth = 0;

    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    ino_entry_t *re = ino_get_nolock(root_ino, NULL);
    if (!re) { spinlock_unlock_irqrestore(&fs_lock, flags); return KZFS_ENOENT; }
    struct vnode *cur = NULL;
    if (vnode_wrap_locked(re, &cur) != KZFS_EOK) {
        ino_put_nolock(re);
        spinlock_unlock_irqrestore(&fs_lock, flags);
        return KZFS_ENOMEM;
    }
    // Referensi dari ino_get_nolock KINI milik vnode — tidak di-put di sini.
    spinlock_unlock_irqrestore(&fs_lock, flags);
    stack[depth++] = cur;                            // stack[0] = root

    int rc = KZFS_EOK;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;                       // komponen kosong / root
        if (!*p) break;                              // selesai (termasuk "a/")

        const char *s = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - s);
        if (len > KZFS_NAME_MAX) { rc = KZFS_ENAMETOOLONG; break; }

        if (len == 1 && s[0] == '.') continue;       // "." = no-op
        if (len == 2 && s[0] == '.' && s[1] == '.') { // ".." = naik satu level
            if (depth > 1) {
                cur->ops->release(cur);              // lepas level yang ditinggalkan
                depth--;
                cur = stack[depth - 1];              // ref milik stack, tetap hidup
            }
            continue;                                // di root: tetap root
        }
        if (depth > KZFS_PATH_MAX_DEPTH) { rc = KZFS_EINVAL; break; }

        char comp[KZFS_NAME_MAX + 1];
        memcpy(comp, s, len);
        comp[len] = '\0';

        if (cur->type != V_DIR) { rc = KZFS_ENOTDIR; break; }
        struct vnode *next = NULL;
        rc = cur->ops->lookup(cur, comp, &next);
        if (rc != KZFS_EOK || !next) {
            if (rc == KZFS_EOK) rc = KZFS_ENOENT;
            break;
        }
        stack[depth++] = next;
        cur = next;
    }

    if (rc != KZFS_EOK) {
        for (uint32_t i = 0; i < depth; i++) stack[i]->ops->release(stack[i]);
        return rc;
    }
    // Lepas semua ancestor; vnode hasil (stack[depth-1]) memegang ref terakhir.
    for (uint32_t i = 0; i + 1 < depth; i++) stack[i]->ops->release(stack[i]);
    *out = stack[depth - 1];
    return KZFS_EOK;
}

// Pecah path jadi (parent_vnode, nama terakhir). Parent ber-refcount 1.
// Trailing '/' dinormalkan ("a/b/" == "a/b"); nama "." / ".." ditolak.
int kfs_walk_parent(const char *path, char *name_out, uint32_t name_cap,
                    struct vnode **out_parent) {
    if (name_out && name_cap) name_out[0] = '\0';
    if (out_parent) *out_parent = NULL;
    if (!path || !name_out || !name_cap || !out_parent) return KZFS_EINVAL;

    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;         // buang slash ekor
    if (n == 0) return KZFS_EINVAL;

    size_t start = n;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t len = n - start;
    if (len == 0) return KZFS_EINVAL;                // "/" → root, tanpa nama
    if (len == 1 && path[start] == '.') return KZFS_EINVAL;
    if (len == 2 && path[start] == '.' && path[start + 1] == '.') return KZFS_EINVAL;
    if (len + 1 > name_cap) return KZFS_ENAMETOOLONG;
    memcpy(name_out, path + start, len);
    name_out[len] = '\0';

    // Parent = prefix sebelum komponen terakhir. Tanpa '/' → root (KyuzenOS
    // tidak punya cwd; nama bare selalu relatif terhadap root).
    if (start == 0) return kfs_walk_path("/", out_parent);
    char pp[512];
    if (start >= sizeof(pp)) return KZFS_ENAMETOOLONG;
    memcpy(pp, path, start);                         // termasuk '/' pemisah
    pp[start] = '\0';
    return kfs_walk_path(pp, out_parent);
}

// =====================================================================
// INODE LIFECYCLE (create/free) — caller holds fs_lock
// =====================================================================
// Tulis ulang entri ".." di block pertama direktori (folder dipindah oleh
// rename). Layout . / .. ditulis dir_init_dots: "." di offset 0 lalu ".."
// tepat setelah rec_len entri ".". Return KZFS_EOK / -EIO bila layout rusak.
int dir_set_dotdot(kzfs_v4_inode_mem_t *dir, uint32_t parent_ino) {
    uint8_t *blk = (uint8_t*)kmalloc(KZFS_BLOCK_SIZE);
    if (!blk) return KZFS_ENOMEM;

    uint32_t got = 0;
    int rc = blk_read_file(dir, 0, blk, KZFS_BLOCK_SIZE, &got);
    if (rc != KZFS_EOK || got < (uint32_t)KZFS_DIRENT_MIN_REC) { kfree(blk); return KZFS_EIO; }

    uint16_t dot_rec = 0;
    memcpy(&dot_rec, blk + 4, 2);
    if (dot_rec < KZFS_DIRENT_MIN_REC ||
        (uint32_t)dot_rec + KZFS_DIRENT_MIN_REC > KZFS_BLOCK_SIZE) { kfree(blk); return KZFS_EIO; }

    struct kzfs_dir_entry dd;
    memcpy(&dd, blk + dot_rec, 8 + 2);
    if (dd.name_len != 2 || dd.name[0] != '.' || dd.name[1] != '.') { kfree(blk); return KZFS_EIO; }
    dd.inode_num = parent_ino;
    memcpy(blk + dot_rec, &dd, 8 + 2);

    uint32_t w = 0;
    rc = blk_write_file(dir, dot_rec, blk + dot_rec, 8 + 2, &w);
    kfree(blk);
    return rc;
}

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

// =====================================================================
// RENAME (pindah / ganti nama entri) — caller TIDAK memegang fs_lock
// =====================================================================
// Ino parent sebuah direktori lewat entri ".." on-disk (0 = gagal / rusak).
static uint32_t dir_parent_ino(kzfs_v4_inode_mem_t *dir) {
    uint32_t pino = 0; uint8_t pft = 0;
    if (dir_lookup_entry(dir, "..", &pino, &pft) != KZFS_EOK) return 0;
    return pino;
}

// Lihat kontrak di include/kyuzenfs.h (kfs_rename_path). Catatan urutan:
// dirent BARU ditambahkan sebelum yang lama dihapus. Kalau langkah pertama
// gagal tidak ada yang berubah; urutan sebaliknya bisa membuat inode hidup
// tanpa nama (orphan) hanya karena satu kegagalan I/O.
int kfs_rename_path(const char *old_path, const char *new_path) {
    if (!old_path || !new_path) return KZFS_EINVAL;

    char oname[KZFS_NAME_MAX + 1];
    char nname[KZFS_NAME_MAX + 1];
    struct vnode *odir = NULL;
    struct vnode *ndir = NULL;

    int rc = kfs_walk_parent(old_path, oname, sizeof(oname), &odir);
    if (rc != KZFS_EOK) return rc;
    rc = kfs_walk_parent(new_path, nname, sizeof(nname), &ndir);
    if (rc != KZFS_EOK) { odir->ops->release(odir); return rc; }

    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    kzfs_v4_inode_mem_t *od = (kzfs_v4_inode_mem_t*)odir->fs_data;
    kzfs_v4_inode_mem_t *nd = (kzfs_v4_inode_mem_t*)ndir->fs_data;
    ino_entry_t *ve = NULL;

    uint32_t oino = 0; uint8_t oft = 0;
    rc = dir_lookup_entry(od, oname, &oino, &oft);
    if (rc != KZFS_EOK) goto out;
    if (oino == root_ino) { rc = KZFS_EINVAL; goto out; }   // root tak bisa dipindah

    // Target yang sudah ada ditolak (tidak ada clobber), kecuali menunjuk
    // inode yang sama — itu rename ke path identik = no-op.
    uint32_t nino = 0; uint8_t nft = 0;
    int tex = dir_lookup_entry(nd, nname, &nino, &nft);
    if (tex == KZFS_EOK) { rc = (nino == oino) ? KZFS_EOK : KZFS_EEXIST; goto out; }
    if (tex != KZFS_ENOENT) { rc = tex; goto out; }
    if (nd->mode != KZFS_INODE_FLAG_DIR) { rc = KZFS_ENOTDIR; goto out; }

    ve = ino_get_nolock(oino, &rc);
    if (!ve) goto out;
    int is_dir = (ve->ino.mode == KZFS_INODE_FLAG_DIR);

    // Folder tidak boleh dipindah ke dalam dirinya sendiri / subtree-nya:
    // telusuri rantai ".." dari parent tujuan sampai root.
    if (is_dir) {
        kzfs_v4_inode_mem_t *walk = nd;          // ref milik ndir vnode
        ino_entry_t *held = NULL;                // ref tambahan dari ino_get_nolock
        uint32_t guard = 0;
        for (;;) {
            if (walk->ino == oino) { rc = KZFS_EINVAL; break; }
            uint32_t pino = dir_parent_ino(walk);
            if (pino == 0 || pino == walk->ino) break;       // root / ".." rusak
            if (++guard > KZFS_PATH_MAX_DEPTH + 1) { rc = KZFS_EINVAL; break; }
            if (held) { ino_put_nolock(held); held = NULL; }
            int irc = KZFS_EOK;
            held = ino_get_nolock(pino, &irc);
            if (!held) { rc = KZFS_EINVAL; break; }
            walk = &held->ino;
        }
        if (held) ino_put_nolock(held);
        if (rc != KZFS_EOK) {
            ino_put_nolock(ve);
            ve = NULL;
            goto out;
        }
    }

    int same_dir = (nd->ino == od->ino);

    rc = dir_add_entry(nd, nname, oino, oft);
    if (rc != KZFS_EOK) goto out;

    if (is_dir) {
        rc = dir_set_dotdot(&ve->ino, nd->ino);
        if (rc != KZFS_EOK) { (void)dir_remove_entry(nd, nname); goto out; }
        if (!same_dir) {
            nd->links_count++;                    // ".." child sekarang milik nd
            od->links_count--;
            nd->dirty = od->dirty = 1;
        }
    }

    rc = dir_remove_entry(od, oname);
    if (rc != KZFS_EOK) {
        // Batalkan: kembalikan ".." + links_count, lalu buang dirent baru.
        if (is_dir && !same_dir) {
            (void)dir_set_dotdot(&ve->ino, od->ino);
            nd->links_count--;
            od->links_count++;
            nd->dirty = od->dirty = 1;
        }
        (void)dir_remove_entry(nd, nname);
        goto out;
    }

    ve->ino.dirty = 1;
    od->dirty = 1;
    nd->dirty = 1;
    (void)ino_sync_nolock(&ve->ino);
    (void)ino_sync_nolock(od);
    if (!same_dir) (void)ino_sync_nolock(nd);
    (void)bm_flush();
    (void)ibm_flush();
    (void)sb_save();

out:
    if (ve) ino_put_nolock(ve);
    spinlock_unlock_irqrestore(&fs_lock, flags);
    ndir->ops->release(ndir);
    odir->ops->release(odir);
    return rc;
}
