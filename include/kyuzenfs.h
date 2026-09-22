#ifndef KYUZENFS_H
#define KYUZENFS_H

// =====================================================================
// Shim API — kontrak publik KyuzenFS yang dipakai caller lama (syscall.c,
// elf.c, kernel.c mod-install, apps/kernel_userlib.c).
//
// Sejak V4, implementasinya ada di kernel/fs/kyuzenfs_v4.c (extent-based,
// block-cached, vnode). Format on-disk V4 ada di include/kyuzenfs_v4.h;
// format V3 diarsipkan di legacy/include/kyuzenfs_v3.h.
//
// Konvensi return dipertahankan dari V3:
//   kfs_create_file / kfs_create_folder → 1 sukses, 0 gagal
//   kfs_exists / kfs_read_to_buffer     → 1 / 0
//   kfs_get_file_size                   → ukuran byte (0 = gagal/kosong)
//   kfs_resolve_dir                     → 1 sukses (*out = inode ID), 0 gagal
//   kfs_delete_file                     → KZFS_EOK / kode error negatif
//                                         (dulu void; ADDITIVE — caller
//                                         lama tetap boleh mengabaikannya)
// =====================================================================

#include <stdint.h>

struct vnode;   // include/vnode.h — hasil resolusi path

void kfs_init(void);
void kfs_format(void);
void kfs_list_files(void);
int  kfs_create_file(char* path, char* data, uint32_t size);
void kfs_read_file(char* filename);
int  kfs_delete_file(char* path);   // file ATAU folder kosong (rmdir-style)
int  kfs_exists(char* path);
uint32_t kfs_get_file_size(char* path);
int  kfs_read_to_buffer(char* path, char* out_buffer, uint32_t buffer_capacity);

// Path-aware (folder bertingkat). Semua path absolut ("/apps/test.elf").
// Komponen path maksimal 255 char (KZFS_NAME_MAX). Tidak ada "." / ".."
// sebagai komponen input — keduanya hanya entri on-disk direktori.
int kfs_resolve_dir(char* path, uint32_t* out_dir_id);
int kfs_create_folder(char* path);
int kfs_get_file_list(char* path, void* buffer, int max_entries);

// --- Resolusi path (dipakai lapisan fd di kernel/fs/vfs_fd.c) ---------------
// Satu-satunya jalur resolusi path di KyuzenFS. Semua bentuk path ditangani
// di sini supaya shim kfs_* dan fd API tidak pernah berbeda pendapat:
//
//   "/"  "//"  "/a//b"      → komponen kosong diabaikan
//   "."                     → komponen saat ini (no-op)
//   ".."                    → naik satu level; di root TETAP root (ext2-style)
//   "/a/b/"                 → trailing '/' diabaikan
//   "nama" (tanpa '/')      → relatif terhadap root (tidak ada cwd di KyuzenOS)
//
// Nama komponen > KZFS_NAME_MAX → -ENAMETOOLONG; kedalaman > KZFS_PATH_MAX_DEPTH
// → -EINVAL. Return KZFS_EOK (include/kyuzenfs_v4.h) dan *out = vnode
// ber-refcount 1 (caller WAJIB release), atau kode error negatif:
//   -ENOENT   komponen tidak ada
//   -ENOTDIR  ada komponen non-direktori di tengah path
//   -EIO      filesystem belum termount
int kfs_walk_path(const char* path, struct vnode** out);

// Pecah path jadi (parent, nama komponen terakhir). Parent ber-refcount 1
// (caller release). Dipakai operasi yang membuat/menghapus entri.
// Return KZFS_EOK, atau -EINVAL bila path menunjuk root / berakhir pada
// komponen "." / ".." (tidak ada nama).
int kfs_walk_parent(const char* path, char* name_out, uint32_t name_cap,
                    struct vnode** out_parent);

// --- Operasi tree tingkat path ------------------------------------------
// rename: pindah/ganti nama file atau folder. Struktur on-disk yang ikut
// diperbarui: dirent parent lama/baru, links_count kedua parent, dan entri
// ".." folder yang dipindah. TIDAK menimpa target yang sudah ada.
// Return: KZFS_EOK · -ENOENT (old tidak ada) · -EEXIST (target sudah ada,
// inode berbeda) · -EINVAL (root dipindah / folder dipindah ke subtree-nya
// sendiri / nama "."/"."/"root") · -ENOTDIR (parent tujuan bukan direktori).
int kfs_rename_path(const char* old_path, const char* new_path);

// stat minimal untuk user-space: ukuran byte + apakah direktori (1/0).
// Kedua out-param boleh NULL. Return KZFS_EOK atau -ENOENT / -ENOTDIR.
int kfs_v4_stat(const char* path, uint32_t* out_size, uint8_t* out_is_dir);

// Statistik V4 (dipakai taskmgr / diagnostik).
uint32_t kfs_get_total_space(void);
uint32_t kfs_get_used_space(void);

// Sinkronisasi global: bitmap + superblock + bcache dirty → disk.
// Dipanggil timer berkala (3 detik) dan sebelum poweroff/reboot.
void kfs_sync_all(void);

// Versi BEST-EFFORT untuk jalur panic: TIDAK menunggu lock FS/bcache. Kalau
// lock sedang dipegang (termasuk oleh CPU yang fault — self-deadlock), sync
// dilewati dan return 0. Lebih baik kehilangan beberapa block dirty daripada
// auto-reboot yang menggantung selamanya setelah BSOD.
int  kfs_sync_all_try(void);

// Diagnostik V4.
int  kfs_v4_is_mounted(void);
void kfs_v4_get_stats(uint64_t* total_blocks, uint64_t* free_blocks,
                      uint32_t* total_inodes, uint32_t* free_inodes);
void kfs_v4_cache_stats(uint64_t* hit, uint64_t* miss);
struct vnode* kfs_v4_root_vnode(void);

#endif // KYUZENFS_H
