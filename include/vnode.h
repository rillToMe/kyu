#ifndef VNODE_H
#define VNODE_H

// =====================================================================
// VNode — abstraksi file/direktori generik yang memisahkan VFS (layer fd
// per-task di kernel/vfs_fd.c) dari concrete filesystem (KyuzenFS V4 di
// kernel/fs/kyuzenfs_v4.c).
//
// Kontrak:
//   * Semua operasi return 0 sukses atau kode error negatif POSIX-style
//     (-ENOENT dst — lihat include/kyuzenfs_v4.h).
//   * Operasi read/write memakai offset eksplisit dan mengembalikan jumlah
//     byte aktual lewat out-param — tidak pernah buffer seluruh file.
//   * vnode hidup di bawah global fs vnode lock (diimplementasi FS concrete);
//     refcount dihitung per handle fd yang menunjuk ke vnode yang sama.
//   * Device (TTY) diekspos sebagai vnode V_DEV oleh shim vnode_dev.c.
// =====================================================================

#include <stdint.h>

typedef enum { V_REG, V_DIR, V_DEV } vnode_type_t;

struct vnode;

struct vnode_ops {
    int (*open)(struct vnode *node, int flags);
    // Baca count byte mulai offset → bytes_read (short read di EOF = OK).
    int (*read)(struct vnode *node, uint64_t offset, void *buf,
                uint64_t count, uint64_t *bytes_read);
    // Tulis count byte mulai offset → bytes_written; memperbesar file bila
    // offset+count > size (in-place pada block fisik yang sudah ada).
    int (*write)(struct vnode *node, uint64_t offset, const void *buf,
                 uint64_t count, uint64_t *bytes_written);
    // Cari nama di direktori → vnode hasil (refcount 1) atau -ENOENT.
    int (*lookup)(struct vnode *dir_node, const char *name,
                  struct vnode **result);
    // Buat file reguler baru bernama name di dir_node → vnode (refcount 1).
    int (*create)(struct vnode *dir_node, const char *name, uint16_t mode,
                  struct vnode **result);
    // Potong/perbesar file ke new_size (0 = truncate).
    int (*truncate)(struct vnode *node, uint64_t new_size);
    // Buat entri direktori baru bernama name di dir_node.
    int (*mkdir)(struct vnode *dir_node, const char *name);
    // Buang entri name dari dir_node (-ENOENT jika tidak ada; folder kosong
    // saja untuk rmdir-style: entri folder dihapus, blok diklaim balik).
    int (*unlink)(struct vnode *dir_node, const char *name);
    // Enumerasi isi direktori: entri ke-N (by index) → name/type.
    // Return 0 = ada, -ENOENT = habis. Dipakai get_file_list.
    int (*readdir)(struct vnode *dir_node, uint32_t index,
                   char *name_out, uint32_t name_cap, uint8_t *type_out);
    // Tulis balik metadata inode (size/extents/times) ke disk.
    int (*sync)(struct vnode *node);
    // Turunkan refcount; refcount 0 → metadata flush + vnode dibebaskan.
    void (*release)(struct vnode *node);
};

struct vnode {
    vnode_type_t    type;
    uint32_t        refcount;    // handle fd yang menunjuk vnode ini
    uint64_t        size;        // ukuran file aktual (byte)
    uint32_t        inode_num;   // nomor inode FS concrete (0 = anon/dev)
    struct vnode_ops *ops;
    void           *fs_data;     // internal FS (kzfs_v4_inode_mem_t)
};

#endif // VNODE_H
