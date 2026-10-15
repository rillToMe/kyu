#include "syscall.h"
#include <stdint.h>
#include "task.h"
#include "usercopy.h"
#include "kz_shm.h"
#include "kz_surface.h"   // KZ_EINVAL for the null-task guard

// R3 shared surfaces (syscalls 88, 91-94). All ops act on the CALLER's own
// address space (st->pml4_phys); a task can never map/unmap another AS.
// Handles are (generation, slot): forgery yields KZ_ESTALE at best.

int sys_shm_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    uint64_t syscall_num = r->rax;
    int32_t task;
    uint64_t pml4;

    (void)uc;
    if (!st || st->pml4_phys == PHYS_NULL) {
        *ret = (uint64_t)(int64_t)KZ_EINVAL;
        return 0;
    }
    task = (int32_t)st->id;
    pml4 = (uint64_t)st->pml4_phys;

    if (syscall_num == SYS_SHM_CREATE) {
        *ret = (uint64_t)kz_shm_create(task, pml4, (uint32_t)r->rbx,
                                       (uint32_t)r->rcx, (uint32_t)r->rdx);
    } else if (syscall_num == SYS_SHM_MAP) {
        *ret = kz_shm_map(task, pml4, r->rbx, (uint32_t)r->rcx);
    } else if (syscall_num == SYS_SHM_UNMAP) {
        *ret = (uint64_t)(int64_t)kz_shm_unmap(task, pml4, r->rbx);
    } else if (syscall_num == SYS_SHM_PUBLISH) {
        *ret = (uint64_t)(int64_t)kz_shm_publish(task, r->rbx, (int32_t)r->rcx,
                                                 (int32_t)r->rdx,
                                                 (int32_t)r->rsi,
                                                 (int32_t)r->rdi);
    } else if (syscall_num == SYS_SHM_DESTROY) {
        *ret = (uint64_t)(int64_t)kz_shm_destroy(task, r->rbx);
    } else if (syscall_num == SYS_SHM_STAT) {
        // Validate the out-pointer BEFORE touching the resource (nothing is
        // consumed on failure, mirroring the sys_get_event pattern).
        if (!user_range_ok(uc, r->rcx, sizeof(kz_shm_stat_t))) {
            *ret = (uint64_t)(int64_t)KZ_EINVAL;
        } else {
            kz_shm_stat_t st;
            int32_t rc = kz_shm_stat(task, r->rbx, &st);
            if (rc == KZ_OK) copy_to_user(uc, r->rcx, &st, sizeof(st));
            *ret = (uint64_t)(int64_t)rc;
        }
    } else if (syscall_num == SYS_SHM_ACQUIRE) {
        *ret = (uint64_t)(int64_t)kz_shm_acquire(task, r->rbx);
    } else if (syscall_num == SYS_SHM_RELEASE) {
        *ret = (uint64_t)(int64_t)kz_shm_release(task, r->rbx);
    }

    return 0;
}
