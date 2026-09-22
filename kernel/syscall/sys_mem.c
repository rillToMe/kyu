#include "syscall.h"
#include <stdint.h>
#include "task.h"
#include "usercopy.h"
#include "uheap.h"
#include "heap.h"

int sys_mem_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    uint64_t syscall_num = r->rax;

    if (syscall_num == 9) { // sys_alloc
        // FIX_005 Tahap 3: ring 3 → region user range milik AS caller
        // (kernel/mm/uheap.c) — pointer yang lewat boundary bukan lagi alamat
        // heap kernel. Ring 0 (shell/login/zen) tetap kmalloc.
        if (uc->from_user) {
            *ret = uheap_alloc(st, r->rbx);
        } else {
            *ret = (uint64_t)kmalloc((uint32_t)r->rbx);
        }
    }
    else if (syscall_num == 10) { // sys_free
        // Tahap 3: kepemilikan divalidasi — pointer asing/stale diabaikan,
        // metadata allocator hidup di heap kernel (tak tersentuh app).
        if (uc->from_user) {
            uheap_free(st, r->rbx);
        } else {
            kfree((void*)r->rbx);
        }
    }
    else if (syscall_num == 19) { // sys_realloc
        // Tahap 3 ring 3: ukuran lama dilacak kernel per region — argumen
        // old_size (rcx) dari app tidak dipercaya lagi.
        if (uc->from_user) {
            *ret = uheap_realloc(st, r->rbx, r->rdx);
        } else {
            *ret = (uint64_t)krealloc((void*)r->rbx, (uint32_t)r->rcx, (uint32_t)r->rdx);
        }
    }

    return 0;
}
