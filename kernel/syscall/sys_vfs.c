#include "syscall.h"
#include <stdint.h>
#include "task.h"
#include "usercopy.h"
#include "vfs.h"         // vfs_* API + SYS_DUP/SYS_DUP2/SYS_PIPE/SYS_READDIR
#include "kyuzenfs_v4.h" // KZFS_NAME_MAX (batas nama sys_readdir)
#include "heap.h"

int sys_vfs_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    (void)st;
    uint64_t syscall_num = r->rax;

    if (syscall_num == 47) { // sys_open(path, flags) -> fd
        char kpath[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kpath, r->rbx, sizeof(kpath)) > 0) {
            *ret = (uint64_t)(int64_t)vfs_open(kpath, (uint32_t)r->rcx);
        } else {
            *ret = (uint64_t)(int64_t)-1;
        }
    }
    else if (syscall_num == 48) { // sys_read(fd, buf, count) -> bytes
        // Tahap 2: baca ke bounce kernel, copy-out di luar vfs_lock.
        uint32_t count = (uint32_t)r->rdx;
        if (count > UC_MAX_IO) count = UC_MAX_IO;
        int n = -1;
        if (count == 0) {
            uint8_t dummy;
            n = vfs_read((int)r->rbx, &dummy, 0);   // pertahankan validasi fd
        } else if (user_range_ok(uc, r->rcx, count)) {
            uint8_t* bounce = (uint8_t*)kmalloc(count);
            if (bounce) {
                n = vfs_read((int)r->rbx, bounce, count);
                if (n > 0) copy_to_user(uc, r->rcx, bounce, (uint32_t)n);
                kfree(bounce);
            }
        }
        *ret = (uint64_t)(int64_t)n;
    }
    else if (syscall_num == 49) { // sys_write(fd, buf, count) -> bytes
        // Tahap 2: copy-in ke bounce kernel; count > UC_MAX_IO ditolak
        // eksplisit (dulu count liar = OOB read + alokasi tak berbatas).
        uint32_t count = (uint32_t)r->rdx;
        int n = -1;
        if (count == 0) {
            uint8_t dummy = 0;
            n = vfs_write((int)r->rbx, &dummy, 0);
        } else if (count <= UC_MAX_IO && user_range_ok(uc, r->rcx, count)) {
            uint8_t* bounce = (uint8_t*)kmalloc(count);
            if (bounce) {
                copy_from_user(uc, bounce, r->rcx, count); // range sudah valid
                n = vfs_write((int)r->rbx, bounce, count);
                kfree(bounce);
            }
        }
        *ret = (uint64_t)(int64_t)n;
    }
    else if (syscall_num == 50) { // sys_lseek(fd, offset, whence) -> pos
        *ret = (uint64_t)(int64_t)vfs_lseek((int)r->rbx, (int32_t)r->rcx, (int)r->rdx);
    }
    else if (syscall_num == 51) { // sys_close(fd) -> 0/-1
        *ret = (uint64_t)(int64_t)vfs_close((int)r->rbx);
    }
    else if (syscall_num == SYS_DUP) { // sys_dup(oldfd) -> newfd / -1
        // P0 Phase 4: newfd shares oldfd's open description (one offset).
        *ret = (uint64_t)(int64_t)vfs_dup((int)r->rbx);
    }
    else if (syscall_num == SYS_DUP2) { // sys_dup2(oldfd, newfd) -> newfd / -1
        // oldfd == newfd is a validated no-op; open newfd closed first.
        *ret = (uint64_t)(int64_t)vfs_dup2((int)r->rbx, (int)r->rcx);
    }
    // ============================================================
    // Fase 4 — filesystem tree (syscall 81-83). Semua path di-copy ke kernel
    // via strncpy_from_user; tidak ada pointer user yang dipakai setelahnya.
    // ============================================================
    else if (syscall_num == SYS_READDIR) { // sys_readdir(fd, index, name, cap, is_dir*)
        uint32_t cap = (uint32_t)r->rsi;
        int ok = -1;
        if (cap > (uint32_t)(KZFS_NAME_MAX + 1)) cap = (uint32_t)(KZFS_NAME_MAX + 1);
        if (cap > 0 && user_range_ok(uc, r->rdx, cap)) {
            char kname[KZFS_NAME_MAX + 1];
            uint8_t kdir = 0;
            if (vfs_readdir((int)r->rbx, (uint32_t)r->rcx, kname, sizeof(kname), &kdir) == 0) {
                // Nama mentah bisa sepanjang KZFS_NAME_MAX; potong ke kapasitas
                // buffer user (selalu NUL-terminated, <= cap byte).
                uint32_t n = 0;
                while (n + 1 < cap && kname[n]) n++;
                kname[n] = '\0';
                if (copy_to_user(uc, r->rdx, kname, n + 1) == 0) {
                    if (r->rdi) (void)copy_to_user(uc, r->rdi, &kdir, 1);
                    ok = 0;
                }
            }
        }
        *ret = (uint64_t)(int64_t)ok;
    }
    else if (syscall_num == SYS_PIPE) { // sys_pipe(fds) -> 0 / -1
        // P0 Phase 5: RBX=user int[2]. Both-or-neither: kernel bounce
        // pair filled by vfs_pipe, copied out only on success.
        int kf[2] = { -1, -1 };
        int ok = -1;
        if (user_range_ok(uc, r->rbx, sizeof(kf))) {
            ok = vfs_pipe(kf);
            if (ok == 0 && copy_to_user(uc, r->rbx, kf, sizeof(kf)) != 0) {
                // Copy-out failed after creation (caller address went bad):
                // close both ends rather than leak them into the task.
                vfs_close(kf[0]);
                vfs_close(kf[1]);
                ok = -1;
            }
        }
        *ret = (uint64_t)(int64_t)ok;
    }

    return 0;
}
