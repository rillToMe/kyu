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
// =====================================================================

#include <stdint.h>

void kfs_init(void);
void kfs_format(void);
void kfs_list_files(void);
int  kfs_create_file(char* path, char* data, uint32_t size);
void kfs_read_file(char* filename);
void kfs_delete_file(char* path);
int  kfs_exists(char* path);
uint32_t kfs_get_file_size(char* path);
int  kfs_read_to_buffer(char* path, char* out_buffer, uint32_t buffer_capacity);

// Path-aware (folder bertingkat). Semua path absolut ("/apps/test.elf").
// Komponen path maksimal 255 char (KZFS_NAME_MAX). Tidak ada "." / ".."
// sebagai komponen input — keduanya hanya entri on-disk direktori.
int kfs_resolve_dir(char* path, uint32_t* out_dir_id);
int kfs_create_folder(char* path);
int kfs_get_file_list(char* path, void* buffer, int max_entries);

// Statistik V4 (dipakai taskmgr / diagnostik).
uint32_t kfs_get_total_space(void);
uint32_t kfs_get_used_space(void);

// Sinkronisasi global: bitmap + superblock + bcache dirty → disk.
// Dipanggil timer berkala (3 detik) dan sebelum poweroff/reboot.
void kfs_sync_all(void);

// Diagnostik V4.
int  kfs_v4_is_mounted(void);
void kfs_v4_get_stats(uint64_t* total_blocks, uint64_t* free_blocks,
                      uint32_t* total_inodes, uint32_t* free_inodes);
void kfs_v4_cache_stats(uint64_t* hit, uint64_t* miss);
struct vnode* kfs_v4_root_vnode(void);

#endif // KYUZENFS_H
