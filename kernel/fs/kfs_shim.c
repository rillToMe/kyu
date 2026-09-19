// kernel/fs/kfs_shim.c — KyuzenFS V4: shim kompatibilitas API lama kfs_*
// (split dari kyuzenfs_v4.c).
//
// Dipakai caller lama: syscall.c, elf.c, kernel.c, apps/kernel_userlib.c.
// Konvensi return V3 dipertahankan:
//   create_file/create_folder → 1 sukses, 0 gagal
//   exists/read_to_buffer     → 1 / 0
//   get_file_size             → ukuran byte (0 = gagal/kosong)
//   resolve_dir               → 1 sukses (*out = inode ID), 0 gagal

#include "kfs_internal.h"
#include "kyuzenfs.h"

static int shim_stat(const char *path, kzfs_v4_inode_mem_t *out, int *is_dir) {
    struct vnode *vn = kfs_walk(path);
    if (!vn) return 0;
    memcpy(out, (kzfs_v4_inode_mem_t*)vn->fs_data, sizeof(*out));
    if (is_dir) *is_dir = (vn->type == V_DIR);
    vn->ops->release(vn);
    return 1;
}

int kfs_exists(char *path) {
    if (!fs_mounted) return 0;
    kzfs_v4_inode_mem_t in; int is_dir = 0;
    return shim_stat(path, &in, &is_dir);
}

uint32_t kfs_get_file_size(char *path) {
    kzfs_v4_inode_mem_t in; int is_dir = 0;
    if (!shim_stat(path, &in, &is_dir)) return 0;
    return (uint32_t)in.size_bytes;
}

int kfs_create_file(char *path, char *data, uint32_t size) {
    const char *name;
    struct vnode *parent = kfs_walk_parent(path, &name);
    if (!parent) return 0;
    int rc = 0;
    struct vnode *vn = NULL;
    if (parent->ops->create(parent, name, 0, &vn) == KZFS_EOK && vn) {
        rc = 1;
        if (size > 0 && data) {
            uint64_t w = 0;
            if (vn->ops->write(vn, 0, data, size, &w) != KZFS_EOK || w != size) {
                (void)vn->ops->truncate(vn, 0);
                vn->ops->release(vn);
                (void)parent->ops->unlink(parent, name);
                rc = 0;
            } else {
                vn->ops->release(vn);
            }
        } else {
            vn->ops->release(vn);
        }
    }
    parent->ops->release(parent);
    return rc;
}

int kfs_create_folder(char *path) {
    const char *name;
    struct vnode *parent = kfs_walk_parent(path, &name);
    if (!parent) return 0;
    int rc = (parent->ops->mkdir(parent, name) == KZFS_EOK) ? 1 : 0;
    parent->ops->release(parent);
    return rc;
}

int kfs_read_to_buffer(char *path, char *out_buffer, uint32_t buffer_capacity) {
    kzfs_v4_inode_mem_t in; int is_dir = 0;
    if (!shim_stat(path, &in, &is_dir) || is_dir) return 0;
    if (in.size_bytes > buffer_capacity) return 0;   // semantik V3
    struct vnode *vn = kfs_walk(path);
    if (!vn) return 0;
    uint64_t got = 0;
    int rc = 0;
    if (vn->ops->read(vn, 0, out_buffer, in.size_bytes, &got) == KZFS_EOK &&
        got == in.size_bytes) rc = 1;
    vn->ops->release(vn);
    return rc;
}

void kfs_delete_file(char *path) {
    const char *name;
    struct vnode *parent = kfs_walk_parent(path, &name);
    if (!parent) return;
    if (parent->ops->unlink(parent, name) != KZFS_EOK)
        kprint("[KZFS4] delete gagal\n");
    parent->ops->release(parent);
}

int kfs_resolve_dir(char *path, uint32_t *out_dir_sector) {
    // Semantik V3: ID direktori. V4 tidak berbasis sektor; inode number
    // dipakai sebagai ID (root = 1, dipakai caller untuk perbandingan).
    kzfs_v4_inode_mem_t in; int is_dir = 0;
    if (!shim_stat(path, &in, &is_dir) || !is_dir) return 0;
    if (out_dir_sector) *out_dir_sector = in.ino;
    return 1;
}

int kfs_get_file_list(char *path, void *buffer, int max_entries) {
    // Layout file_info_t dipertahankan (include/userlib.h):
    // { char filename[24]; uint32_t size; uint8_t is_folder; }
    typedef struct { char filename[24]; uint32_t size; uint8_t is_folder; } shim_info_t;
    shim_info_t *list = (shim_info_t*)buffer;
    if (!buffer || max_entries <= 0) return 0;

    struct vnode *dir = kfs_walk(path);
    if (!dir || dir->type != V_DIR) {
        if (dir) dir->ops->release(dir);
        return 0;
    }
    char nm[KZFS_NAME_MAX + 2]; uint8_t ty;
    int count = 0;
    for (uint32_t i = 0; count < max_entries; i++) {
        if (dir->ops->readdir(dir, i, nm, sizeof(nm), &ty) != KZFS_EOK) break;
        if (dir_is_dot(nm)) continue;            // lewati ".", ".."
        int j = 0;
        while (j < 22 && nm[j]) { list[count].filename[j] = nm[j]; j++; }
        list[count].filename[j] = '\0';
        list[count].is_folder = (ty == KZFS_INODE_FLAG_DIR) ? 1 : 0;
        list[count].size = 0;
        if (ty != KZFS_INODE_FLAG_DIR) {
            struct vnode *child = NULL;
            if (dir->ops->lookup(dir, nm, &child) == KZFS_EOK && child) {
                list[count].size = (uint32_t)child->size;
                child->ops->release(child);
            }
        }
        count++;
    }
    dir->ops->release(dir);
    return count;
}

void kfs_list_files(void) {
    struct vnode *root = kfs_walk("/");
    if (!root) { kprint("Disk belum termount!\n"); return; }
    kprint("--- / (KyuzenFS V4) ---\n");
    char nm[KZFS_NAME_MAX + 2]; uint8_t ty;
    for (uint32_t i = 0; ; i++) {
        if (root->ops->readdir(root, i, nm, sizeof(nm), &ty) != KZFS_EOK) break;
        if (dir_is_dot(nm)) continue;
        kprint(ty == KZFS_INODE_FLAG_DIR ? "d " : "- ");
        kprint(nm);
        kprint("\n");
    }
    root->ops->release(root);
}

void kfs_read_file(char *filename) {
    uint32_t size = kfs_get_file_size(filename);
    if (size == 0) { kprint("File tidak ada / kosong.\n"); return; }
    char *buf = (char*)kmalloc(size + 1);
    if (!buf) { kprint("Heap habis.\n"); return; }
    if (kfs_read_to_buffer(filename, buf, size)) {
        buf[size] = '\0';
        uint32_t off = 0;
        while (off < size) {
            uint32_t seg = 0;
            while (off + seg < size && buf[off + seg] != '\0') seg++;
            if (seg == 0) { kprint("\\0"); off++; continue; }
            buf[off + seg] = '\0';
            kprint(buf + off);
            off += seg;
        }
        kprint("\n");
    } else {
        kprint("Gagal membaca file.\n");
    }
    kfree(buf);
}

uint32_t kfs_get_total_space(void) {
    uint64_t total = 0;
    kfs_v4_get_stats(&total, NULL, NULL, NULL);
    return (uint32_t)(total * KZFS_BLOCK_SIZE);
}

uint32_t kfs_get_used_space(void) {
    uint64_t total = 0, free_b = 0;
    kfs_v4_get_stats(&total, &free_b, NULL, NULL);
    return (uint32_t)((total - free_b) * KZFS_BLOCK_SIZE);
}

// Vnode root (refcount 1 — milik caller). NULL bila belum termount.
struct vnode* kfs_v4_root_vnode(void) {
    if (!fs_mounted) return NULL;
    int rc = KZFS_EOK;
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    ino_entry_t *re = ino_get_nolock(root_ino, &rc);
    struct vnode *v = NULL;
    if (re) {
        rc = vnode_wrap_locked(re, &v);
        if (rc != KZFS_EOK) ino_put_nolock(re);
    }
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return v;
}
