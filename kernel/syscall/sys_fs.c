#include "syscall.h"
#include <stdint.h>
#include "task.h"
#include "usercopy.h"
#include "userlib.h"   // file_info_t (ABI sys_get_file_list)
#include "kyuzenfs.h"  // kfs_* shim API (pengganti extern lokal di syscall.c lama)
#include "vfs.h"       // SYS_RENAME / SYS_STAT
#include "heap.h"

int sys_fs_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    (void)st;
    uint64_t syscall_num = r->rax;

    if (syscall_num == 5) { // sys_fs_format — root only (destruktif)
        if (!cred_current_is_root()) {
            *ret = (uint64_t)-1;
        } else {
            kfs_format();
            *ret = 0;
        }
    }
    else if (syscall_num == 6) { // sys_fs_list
        kfs_list_files();
    }
    else if (syscall_num == 7) { // sys_fs_read
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kf, r->rbx, sizeof(kf)) >= 0) kfs_read_file(kf);
    }
    else if (syscall_num == 8) { // sys_fs_delete — unlink file ATAU rmdir kosong
        // ADDITIVE: return 0 sukses / -1 gagal (dulu void). Folder tidak kosong
        // = -1, jadi caller bisa membedakan "kosong" dari "gagal".
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kf, r->rbx, sizeof(kf)) >= 0) {
            *ret = (kfs_delete_file(kf) == 0) ? 0 : (uint64_t)-1;
        } else {
            *ret = (uint64_t)-1;
        }
    }
    else if (syscall_num == 11) { // sys_file_exists
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kf, r->rbx, sizeof(kf)) >= 0) {
            *ret = kfs_exists(kf);
        }
    }
    else if (syscall_num == 12) { // sys_file_size
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kf, r->rbx, sizeof(kf)) >= 0) {
            *ret = kfs_get_file_size(kf);
        }
    }
    else if (syscall_num == 13) { // sys_read_file_to_buffer
        // Tahap 2: baca ke bounce kernel, copy-out di luar fs_lock.
        // Semantik lama dipertahankan: size > capacity → 0; file ada → 1.
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kf, r->rbx, sizeof(kf)) >= 0) {
            uint32_t cap   = (uint32_t)r->rdx;
            uint32_t fsize = kfs_get_file_size(kf);
            if (fsize == 0) {
                // File kosong / tidak ada — buffer user tidak disentuh.
                *ret = kfs_exists(kf) ? 1 : 0;
            } else if (fsize <= cap && fsize <= UC_MAX_FILE &&
                       user_range_ok(uc, r->rcx, fsize)) {
                char* bounce = (char*)kmalloc(fsize);
                if (bounce) {
                    if (kfs_read_to_buffer(kf, bounce, fsize) &&
                        copy_to_user(uc, r->rcx, bounce, fsize) == 0) {
                        *ret = 1;
                    }
                    kfree(bounce);
                }
            }
        }
    }
    else if (syscall_num == 18) { // sys_create_file
        char kf[UC_MAX_FNAME];
        uint32_t size = (uint32_t)r->rdx;
        if (strncpy_from_user(uc, kf, r->rbx, sizeof(kf)) >= 0 && size <= UC_MAX_FILE) {
            if (size == 0) {
                char empty = '\0';
                *ret = kfs_create_file(kf, &empty, 0);
            } else {
                char* bounce = (char*)kmalloc(size);
                if (bounce) {
                    if (copy_from_user(uc, bounce, r->rcx, size) == 0) {
                        *ret = kfs_create_file(kf, bounce, size);
                    }
                    kfree(bounce);
                }
            }
        }
    }
    else if (syscall_num == 24) { // sys_get_file_list(path, buffer, max_entries)
        // Fase 2: argumen path user-space. RBX=path, RCX=buffer, RDX=maxn.
        // Tulis ke bounce kernel, copy-out setelah fs_lock lepas.
        char kpath[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kpath, r->rbx, sizeof(kpath)) >= 0) {
            int maxn = (int)r->rdx;
            if (maxn > (int)UC_MAX_ENTRIES) maxn = (int)UC_MAX_ENTRIES;
            uint64_t bytes = (uint64_t)maxn * sizeof(file_info_t);
            if (maxn > 0 && user_range_ok(uc, r->rcx, bytes)) {
                file_info_t* bounce = (file_info_t*)kmalloc((uint32_t)bytes);
                if (bounce) {
                    int count = kfs_get_file_list(kpath, bounce, maxn);
                    if (count > 0 &&
                        copy_to_user(uc, r->rcx, bounce,
                                     (uint64_t)count * sizeof(file_info_t)) != 0) {
                        count = 0;
                    }
                    *ret = (uint64_t)count;
                    kfree(bounce);
                }
            }
        }
    }
    else if (syscall_num == 64) { // sys_mkdir(path) — buat folder KyuzenFS
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kf, r->rbx, sizeof(kf)) >= 0) {
            *ret = (uint64_t)kfs_create_folder(kf);
        }
    }
    else if (syscall_num == SYS_RENAME) { // sys_rename(old_path, new_path) -> 0 / -1
        char kold[UC_MAX_FNAME];
        char knew[UC_MAX_FNAME];
        int ok = -1;
        if (strncpy_from_user(uc, kold, r->rbx, sizeof(kold)) >= 0 &&
            strncpy_from_user(uc, knew, r->rcx, sizeof(knew)) >= 0) {
            ok = (kfs_rename_path(kold, knew) == 0) ? 0 : -1;
        }
        *ret = (uint64_t)(int64_t)ok;
    }
    else if (syscall_num == SYS_STAT) { // sys_stat(path, size*, is_dir*) -> 0 / -1
        char kpath[UC_MAX_FNAME];
        int ok = -1;
        if (strncpy_from_user(uc, kpath, r->rbx, sizeof(kpath)) >= 0) {
            uint32_t ksize = 0;
            uint8_t  kdir = 0;
            if (kfs_v4_stat(kpath, &ksize, &kdir) == 0) {
                int good = 1;
                if (r->rcx && copy_to_user(uc, r->rcx, &ksize, sizeof(ksize)) != 0) good = 0;
                if (r->rdx && copy_to_user(uc, r->rdx, &kdir, sizeof(kdir)) != 0) good = 0;
                if (good) ok = 0;
            }
        }
        *ret = (uint64_t)(int64_t)ok;
    }

    return 0;
}
