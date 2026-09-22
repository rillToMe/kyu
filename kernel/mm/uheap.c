// kernel/mm/uheap.c — FIX_005 Tahap 3: heap user per proses.
//
// Allocator region page-granular di user range AS pemanggil. Metadata
// (linked list region) hidup di HEAP KERNEL — app tidak pernah bisa
// menyentuhnya, jadi sys_free liar hanya bisa gagal validasi, bukan
// merusak allocator.
//
// Tanpa lock: state per-task, dan hanya task pemilik (di dalam syscall-nya
// sendiri) yang menyentuh field uheap_*. Lihat kontrak di uheap.h.

#include "uheap.h"
#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "smap.h"

extern uint64_t hhdm_offset;

typedef struct uheap_region {
    uint64_t base;    // alamat user awal region
    uint64_t size;    // ukuran yang diminta app (byte)
    uint64_t pages;   // jumlah halaman ter-map (kapasitas = pages*4096)
    struct uheap_region* next;
} uheap_region_t;

// Unmap + kembalikan frame [base, base + pages*4096) dari AS task.
static void uheap_unmap_range(task_t* t, uint64_t base, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) {
        phys_addr_t pa = vmm_unmap_page_from(base + i * 4096, t->pml4_phys);
        if (pa != PHYS_NULL && pmm_owns_page(pa)) pmm_free_page(pa);
    }
}

uint64_t uheap_alloc(task_t* t, uint64_t size) {
    if (!t || t->pml4_phys == PHYS_NULL) return 0;
    if (size == 0 || size > UHEAP_MAX_ALLOC) return 0;

    uint64_t brk = t->uheap_brk ? t->uheap_brk : UHEAP_BASE;
    uint64_t pages = (size + 4095) / 4096;

    // +1 halaman guard (tidak di-map) antar region — overflow app langsung #PF.
    if (brk + pages * 4096 + 4096 > UHEAP_END) return 0;

    uheap_region_t* node = (uheap_region_t*)kmalloc(sizeof(uheap_region_t));
    if (!node) return 0;

    // Map + zero tiap halaman via HHDM (frame bekas tidak boleh bocor ke app).
    for (uint64_t i = 0; i < pages; i++) {
        phys_addr_t pa = pmm_alloc_page();
        if (pa == PHYS_NULL ||
            !vmm_map_page_into(brk + i * 4096, pa, 7, t->pml4_phys)) {
            if (pa != PHYS_NULL) pmm_free_page(pa);
            uheap_unmap_range(t, brk, i);   // rollback halaman yang sudah ter-map
            kfree(node);
            return 0;
        }
        memset((void*)(pa + hhdm_offset), 0, 4096);
    }

    node->base  = brk;
    node->size  = size;
    node->pages = pages;
    node->next  = (uheap_region_t*)t->uheap_regions;
    t->uheap_regions = node;
    t->uheap_brk = brk + pages * 4096 + 4096;
    return brk;
}

int uheap_free(task_t* t, uint64_t uaddr) {
    if (!t || uaddr == 0) return 0;

    uheap_region_t** link = (uheap_region_t**)&t->uheap_regions;
    for (uheap_region_t* n = *link; n; link = &n->next, n = n->next) {
        if (n->base != uaddr) continue;
        uheap_unmap_range(t, n->base, n->pages);
        *link = n->next;
        kfree(n);
        return 1;
    }
    return 0;   // pointer asing/stale — abaikan
}

uint64_t uheap_realloc(task_t* t, uint64_t uaddr, uint64_t new_size) {
    if (!t) return 0;
    if (uaddr == 0) return uheap_alloc(t, new_size);
    if (new_size == 0) { uheap_free(t, uaddr); return 0; }
    if (new_size > UHEAP_MAX_ALLOC) return 0;

    uheap_region_t* n = (uheap_region_t*)t->uheap_regions;
    while (n && n->base != uaddr) n = n->next;
    if (!n) return 0;   // bukan region milik task ini

    // Muat di kapasitas halaman yang sudah ter-map → in-place.
    if (new_size <= n->pages * 4096) {
        n->size = new_size;
        return n->base;
    }

    uint64_t new_base = uheap_alloc(t, new_size);
    if (new_base == 0) return 0;   // region lama tetap utuh

    // Copy via alamat user langsung — CR3 = AS caller selama syscall.
    // Tahap 4: kedua sisi halaman user → butuh jendela SMAP.
    uint64_t copy = (n->size < new_size) ? n->size : new_size;
    user_access_begin();
    memcpy((void*)new_base, (const void*)uaddr, copy);
    user_access_end();
    uheap_free(t, uaddr);
    return new_base;
}

void uheap_reset(task_t* t) {
    if (!t) return;
    uheap_region_t* n = (uheap_region_t*)t->uheap_regions;
    while (n) {
        uheap_region_t* next = n->next;
        kfree(n);
        n = next;
    }
    t->uheap_regions = NULL;
    t->uheap_brk = UHEAP_BASE;
}

int uheap_clone(task_t* dst, const task_t* src) {
    if (!dst || !src) return -1;
    dst->uheap_brk = src->uheap_brk;
    dst->uheap_regions = NULL;
    uheap_region_t** link = (uheap_region_t**)&dst->uheap_regions;
    for (const uheap_region_t* n = (const uheap_region_t*)src->uheap_regions;
         n; n = n->next) {
        uheap_region_t* c = (uheap_region_t*)kmalloc(sizeof(uheap_region_t));
        if (!c) { uheap_reset(dst); return -1; }
        c->base  = n->base;
        c->size  = n->size;
        c->pages = n->pages;
        c->next  = NULL;
        *link = c;
        link = &c->next;
    }
    return 0;
}
