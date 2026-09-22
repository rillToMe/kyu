// kernel/proc/usercopy.c — FIX_005 Tahap 2/3: boundary copy user <-> kernel.
//
// Validasi (Tahap 3, final): alamat harus di USER RANGE (PML4 idx < 256)
// DAN mapped di AS caller. Deref langsung setelah validasi aman karena:
//   - CR3 saat syscall = PML4 caller (int 0x80 tidak mengganti CR3), dan
//   - hanya task pemanggil yang bisa unmap AS-nya sendiri — dan dia sedang
//     berada di dalam syscall ini. (SMAP baru masuk di Tahap 4.)

#include "usercopy.h"
#include "paging.h"
#include "smp.h"
#include "smap.h"
#include <stddef.h>

extern void* memcpy(void* dest, const void* src, size_t n);

void ucopy_ctx_init(ucopy_ctx_t* ctx, const registers_t* r) {
    ctx->from_user = ((r->cs & 3) == 3);
    int task_id = smp_current_task_id();
    ctx->pml4 = (task_id >= 0 && task_id < task_count)
                    ? tasks[task_id].pml4_phys : PHYS_NULL;
}

int user_range_ok(const ucopy_ctx_t* ctx, uint64_t uaddr, uint64_t len) {
    if (uaddr == 0) return 0;
    if (len == 0) return 1;
    if (len > UC_MAX_RANGE) return 0;
    if (uaddr + len < uaddr) return 0;   // wrap alamat
    if (!ctx->from_user) return 1;       // caller ring 0: pointer kernel sah

    // Tahap 3: user range saja — pointer higher-half (heap/HHDM/kernel)
    // ditolak walau mapped di AS caller.
    if (uaddr >= UC_USER_VA_MAX || len > UC_USER_VA_MAX - uaddr) return 0;

    uint64_t first = uaddr & ~0xFFFULL;
    uint64_t last  = (uaddr + len - 1) & ~0xFFFULL;
    for (uint64_t p = first;; p += 0x1000) {
        if (!paging_is_mapped_into(p, ctx->pml4)) return 0;
        if (p == last) break;
    }
    return 1;
}

int copy_from_user(const ucopy_ctx_t* ctx, void* kdst, uint64_t usrc, uint64_t len) {
    if (!user_range_ok(ctx, usrc, len)) return -1;
    // Tahap 4: buka jendela SMAP hanya untuk deref user yang disengaja.
    if (ctx->from_user) user_access_begin();
    memcpy(kdst, (const void*)usrc, len);
    if (ctx->from_user) user_access_end();
    return 0;
}

int copy_to_user(const ucopy_ctx_t* ctx, uint64_t udst, const void* ksrc, uint64_t len) {
    if (!user_range_ok(ctx, udst, len)) return -1;
    if (ctx->from_user) user_access_begin();
    memcpy((void*)udst, ksrc, len);
    if (ctx->from_user) user_access_end();
    return 0;
}

int64_t strncpy_from_user(const ucopy_ctx_t* ctx, char* kdst, uint64_t usrc, uint64_t cap) {
    if (usrc == 0 || cap == 0) return -1;

    uint64_t i = 0;
    if (!ctx->from_user) {
        const char* s = (const char*)usrc;
        while (i < cap - 1 && s[i] != '\0') { kdst[i] = s[i]; i++; }
        kdst[i] = '\0';
        return (int64_t)i;
    }

    // Ring 3: validasi hanya halaman yang benar-benar disentuh — string
    // pendek di halaman terakhir yang mapped harus tetap berhasil.
    // Tahap 4: satu jendela SMAP untuk seluruh loop (fail → tutup dulu).
    int fail = 0;
    uint64_t checked_page = 1;   // bukan page-aligned → pasti != halaman pertama
    user_access_begin();
    while (i < cap - 1) {
        uint64_t addr = usrc + i;
        if (addr < usrc) { fail = 1; break; }   // wrap
        uint64_t page = addr & ~0xFFFULL;
        if (page != checked_page) {
            if (page >= UC_USER_VA_MAX) { fail = 1; break; }   // user range saja
            if (!paging_is_mapped_into(page, ctx->pml4)) { fail = 1; break; }
            checked_page = page;
        }
        char c = *(const char*)addr;
        if (c == '\0') break;
        kdst[i++] = c;
    }
    user_access_end();
    if (fail) return -1;
    kdst[i] = '\0';
    return (int64_t)i;
}
