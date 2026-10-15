#include "kz_shm.h"
#include "kz_surface.h"
#include "pmm.h"
#include "paging.h"
#include "spinlock.h"
#include "string.h"
#include <stddef.h>

extern uint64_t hhdm_offset;

#define KZ_SHM_MAX_MAPS 32
#define KZ_SHM_MAX_REFS 32

typedef struct {
    uint8_t used;
    uint8_t published;
    uint8_t destroyed;
    uint8_t creating;
    uint32_t generation;
    int32_t owner;
    uint32_t width, height, stride, format;
    uint32_t npages;
    phys_addr_t *pages;
    int32_t maps[KZ_SHM_MAX_MAPS];
    uint32_t nmaps;
    int32_t refs[KZ_SHM_MAX_REFS];
    uint32_t nrefs;
    int32_t dx, dy, dw, dh;
} kz_shm_slot_t;

static kz_shm_slot_t g_shm[KZ_SHM_MAX_RES];
static uint32_t g_shm_gen = 1;
static spinlock_t g_shm_lock = SPINLOCK_INIT;

void *kmalloc(size_t size);
void kfree(void *ptr);

static uint64_t shm_flags(int write) {
    uint64_t f = PAGING_PRESENT | PAGING_USER | PAGING_NX;
    if (write) f |= PAGING_WRITABLE;
    return f;
}

static int has_map(kz_shm_slot_t *s, int32_t task) {
    uint32_t i;
    for (i = 0; i < s->nmaps; i++) {
        if (s->maps[i] == task) return 1;
    }
    return 0;
}

static int add_map(kz_shm_slot_t *s, int32_t task) {
    if (s->nmaps >= KZ_SHM_MAX_MAPS) return -1;
    s->maps[s->nmaps++] = task;
    return 0;
}

static void del_map(kz_shm_slot_t *s, int32_t task) {
    uint32_t i;
    for (i = 0; i < s->nmaps; i++) {
        if (s->maps[i] == task) {
            s->maps[i] = s->maps[--s->nmaps];
            return;
        }
    }
}

static int add_ref(kz_shm_slot_t *s, int32_t task) {
    if (s->nrefs >= KZ_SHM_MAX_REFS) return -1;
    s->refs[s->nrefs++] = task;
    return 0;
}

static int has_ref(kz_shm_slot_t *s, int32_t task) {
    uint32_t i;
    for (i = 0; i < s->nrefs; i++) {
        if (s->refs[i] == task) return 1;
    }
    return 0;
}

static void del_ref(kz_shm_slot_t *s, int32_t task) {
    uint32_t i;
    for (i = 0; i < s->nrefs; i++) {
        if (s->refs[i] == task) {
            s->refs[i] = s->refs[--s->nrefs];
            return;
        }
    }
}

static void free_pages(kz_shm_slot_t *s) {
    uint32_t i;
    if (!s->pages) return;
    for (i = 0; i < s->npages; i++) {
        if (s->pages[i]) pmm_free_page(s->pages[i]);
    }
    kfree(s->pages);
    s->pages = NULL;
    s->npages = 0;
}

static void maybe_free(kz_shm_slot_t *s) {
    if (s->destroyed && s->nmaps == 0 && s->nrefs == 0 && !s->creating) {
        free_pages(s);
        s->used = 0;
        s->published = 0;
        s->destroyed = 0;
        s->generation++;
        if (s->generation == 0) s->generation = 1;
    }
}

static kz_status_t lookup(uint64_t h, uint32_t *idx) {
    uint32_t i = kz_shm_idx(h);
    uint32_t g = kz_shm_gen(h);
    if (g == 0 || i >= KZ_SHM_MAX_RES) return KZ_EINVAL;
    if (!g_shm[i].used || g_shm[i].generation != g) return KZ_ESTALE;
    if (g_shm[i].destroyed) return KZ_ESTALE;
    if (idx) *idx = i;
    return KZ_OK;
}

// Releasing your own mapping is always legal, even on a destroyed
// resource (lazy retire): the record is what keeps pages alive, so
// refusing to drop it would leak them forever.
static kz_status_t lookup_unmap(uint64_t h, uint32_t *idx) {
    uint32_t i = kz_shm_idx(h);
    uint32_t g = kz_shm_gen(h);
    if (g == 0 || i >= KZ_SHM_MAX_RES) return KZ_EINVAL;
    if (!g_shm[i].used || g_shm[i].generation != g) return KZ_ESTALE;
    if (idx) *idx = i;
    return KZ_OK;
}

int64_t kz_shm_create(int32_t task, phys_addr_t pml4, uint32_t w, uint32_t h,
                      uint32_t format) {
    kz_surface_desc_t d;
    uint64_t need;
    uint32_t npages, i;
    uint32_t idx = KZ_SHM_MAX_RES;
    uint32_t gen;
    phys_addr_t *pages;
    uint64_t va;

    if (task < 0 || pml4 == PHYS_NULL) return KZ_EINVAL;
    if (format != KZ_FMT_XRGB8888 && format != KZ_FMT_ARGB8888) return KZ_EINVAL;
    d.width = w;
    d.height = h;
    d.stride_px = w;
    d.format = format;
    d.size_bytes = kz_surface_bytes(w, h);
    d.usage = KZ_USAGE_CPU_READ | KZ_USAGE_CPU_WRITE;
    d.id = 0;
    d.generation = 0;
    d.owner = (uint32_t)task;
    if (kz_surface_validate(&d) != KZ_OK) return KZ_EINVAL;
    need = d.size_bytes;
    npages = (uint32_t)((need + KZ_SHM_PAGE_SIZE - 1) / KZ_SHM_PAGE_SIZE);
    if (npages == 0 || npages > KZ_SHM_MAX_PAGES) return KZ_EINVAL;

    spinlock_lock(&g_shm_lock);
    for (i = 0; i < KZ_SHM_MAX_RES; i++) {
        if (!g_shm[i].used) {
            idx = i;
            break;
        }
    }
    if (idx >= KZ_SHM_MAX_RES) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EBUSY;
    }
    gen = g_shm_gen;
    if (gen == 0) gen = 1;
    g_shm_gen = gen + 1;
    if (g_shm_gen == 0) g_shm_gen = 1;
    g_shm[idx].used = 1;
    g_shm[idx].creating = 1;
    g_shm[idx].published = 0;
    g_shm[idx].destroyed = 0;
    g_shm[idx].generation = gen;
    g_shm[idx].owner = task;
    g_shm[idx].nmaps = 0;
    g_shm[idx].nrefs = 0;
    spinlock_unlock(&g_shm_lock);

    pages = (phys_addr_t *)kmalloc(sizeof(phys_addr_t) * npages);
    if (!pages) goto fail;
    for (i = 0; i < npages; i++) {
        phys_addr_t p = pmm_alloc_page();
        if (p == PHYS_NULL) {
            uint32_t j;
            for (j = 0; j < i; j++) pmm_free_page(pages[j]);
            kfree(pages);
            goto fail;
        }
        pages[i] = p;
        memset((void *)(uint64_t)(p + hhdm_offset), 0, 4096);
    }

    va = kz_shm_slot_va(idx);
    for (i = 0; i < npages; i++) {
        if (!vmm_map_page_into(va + (uint64_t)i * 4096, pages[i],
                               shm_flags(1), pml4)) {
            uint32_t j;
            for (j = 0; j < i; j++)
                vmm_unmap_page_from(va + (uint64_t)j * 4096, pml4);
            for (j = 0; j < npages; j++) pmm_free_page(pages[j]);
            kfree(pages);
            goto fail;
        }
    }

    spinlock_lock(&g_shm_lock);
    g_shm[idx].pages = pages;
    g_shm[idx].npages = npages;
    g_shm[idx].width = w;
    g_shm[idx].height = h;
    g_shm[idx].stride = w;
    g_shm[idx].format = format;
    g_shm[idx].creating = 0;
    g_shm[idx].maps[0] = task;
    g_shm[idx].nmaps = 1;
    spinlock_unlock(&g_shm_lock);
    return (int64_t)kz_shm_handle(gen, idx);

fail:
    spinlock_lock(&g_shm_lock);
    g_shm[idx].used = 0;
    g_shm[idx].creating = 0;
    spinlock_unlock(&g_shm_lock);
    return KZ_ENOMEM;
}

uint64_t kz_shm_map(int32_t task, phys_addr_t pml4, uint64_t h, uint32_t flags) {
    uint32_t idx;
    kz_status_t st;
    int write;
    uint32_t i;
    uint64_t va;

    if (task < 0 || pml4 == PHYS_NULL) return 0;
    write = (flags & KZ_SHM_MAP_WRITE) != 0;
    spinlock_lock(&g_shm_lock);
    st = lookup(h, &idx);
    if (st != KZ_OK) {
        spinlock_unlock(&g_shm_lock);
        return 0;
    }
    if (g_shm[idx].creating) {
        spinlock_unlock(&g_shm_lock);
        return 0;
    }
    if (has_map(&g_shm[idx], task)) {
        spinlock_unlock(&g_shm_lock);
        return kz_shm_slot_va(idx);
    }
    if (task != g_shm[idx].owner) {
        if (!g_shm[idx].published) {
            spinlock_unlock(&g_shm_lock);
            return 0;
        }
        if (write) {
            spinlock_unlock(&g_shm_lock);
            return 0;
        }
    }
    if (add_map(&g_shm[idx], task) != 0) {
        spinlock_unlock(&g_shm_lock);
        return 0;
    }
    spinlock_unlock(&g_shm_lock);

    va = kz_shm_slot_va(idx);
    for (i = 0; i < g_shm[idx].npages; i++) {
        if (!vmm_map_page_into(va + (uint64_t)i * 4096, g_shm[idx].pages[i],
                               shm_flags(task == g_shm[idx].owner && write),
                               pml4)) {
            uint32_t j;
            for (j = 0; j < i; j++) {
                vmm_unmap_page_from(va + (uint64_t)j * 4096, pml4);
                vmm_tlb_shootdown(va + (uint64_t)j * 4096);
            }
            spinlock_lock(&g_shm_lock);
            del_map(&g_shm[idx], task);
            maybe_free(&g_shm[idx]);
            spinlock_unlock(&g_shm_lock);
            return 0;
        }
    }
    return va;
}

int32_t kz_shm_unmap(int32_t task, phys_addr_t pml4, uint64_t va) {
    uint32_t idx;
    uint32_t i, npages;

    if (task < 0 || pml4 == PHYS_NULL) return KZ_EINVAL;
    if (va < KZ_SHM_BASE || va >= KZ_SHM_END) return KZ_EINVAL;
    if ((va - KZ_SHM_BASE) % KZ_SHM_SLOT_SIZE != 0) return KZ_EINVAL;
    idx = (uint32_t)((va - KZ_SHM_BASE) / KZ_SHM_SLOT_SIZE);
    if (idx >= KZ_SHM_MAX_RES) return KZ_EINVAL;

    spinlock_lock(&g_shm_lock);
    if (lookup_unmap(kz_shm_handle(g_shm[idx].generation, idx), &idx) !=
        KZ_OK) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EINVAL;
    }
    if (!has_map(&g_shm[idx], task)) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EINVAL;
    }
    npages = g_shm[idx].npages;
    del_map(&g_shm[idx], task);
    maybe_free(&g_shm[idx]);
    spinlock_unlock(&g_shm_lock);

    for (i = 0; i < npages; i++) {
        vmm_unmap_page_from(va + (uint64_t)i * 4096, pml4);
        vmm_tlb_shootdown(va + (uint64_t)i * 4096);
    }
    return KZ_OK;
}

int32_t kz_shm_publish(int32_t task, uint64_t h, int32_t x, int32_t y,
                           int32_t w, int32_t hh) {
    uint32_t idx;
    kz_status_t st;
    uint64_t xe, ye;

    spinlock_lock(&g_shm_lock);
    st = lookup(h, &idx);
    if (st != KZ_OK) {
        spinlock_unlock(&g_shm_lock);
        return st;
    }
    if (task != g_shm[idx].owner) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EACCES;
    }
    if (w < 0 || hh < 0 || x < 0 || y < 0) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EINVAL;
    }
    xe = (uint64_t)x + (uint64_t)w;
    ye = (uint64_t)y + (uint64_t)hh;
    if (xe > g_shm[idx].width || ye > g_shm[idx].height) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EINVAL;
    }
    g_shm[idx].published = 1;
    g_shm[idx].dx = x;
    g_shm[idx].dy = y;
    g_shm[idx].dw = w;
    g_shm[idx].dh = hh;
    spinlock_unlock(&g_shm_lock);
    return KZ_OK;
}

int32_t kz_shm_destroy(int32_t task, uint64_t h) {
    uint32_t idx;
    kz_status_t st;

    spinlock_lock(&g_shm_lock);
    st = lookup(h, &idx);
    if (st != KZ_OK) {
        spinlock_unlock(&g_shm_lock);
        return st;
    }
    if (task != g_shm[idx].owner) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EACCES;
    }
    g_shm[idx].destroyed = 1;
    maybe_free(&g_shm[idx]);
    spinlock_unlock(&g_shm_lock);
    return KZ_OK;
}

void kz_shm_task_exit(int32_t task) {
    uint32_t i;
    if (task < 0) return;
    spinlock_lock(&g_shm_lock);
    for (i = 0; i < KZ_SHM_MAX_RES; i++) {
        if (!g_shm[i].used) continue;
        if (has_map(&g_shm[i], task)) del_map(&g_shm[i], task);
        while (has_ref(&g_shm[i], task)) del_ref(&g_shm[i], task);
        if (g_shm[i].owner == task && !g_shm[i].destroyed)
            g_shm[i].destroyed = 1;
        maybe_free(&g_shm[i]);
    }
    spinlock_unlock(&g_shm_lock);
}

int32_t kz_shm_stat(int32_t task, uint64_t h, kz_shm_stat_t *out) {
    uint32_t idx;
    kz_status_t st;

    if (!out) return KZ_EINVAL;
    spinlock_lock(&g_shm_lock);
    st = lookup(h, &idx);
    if (st != KZ_OK) {
        spinlock_unlock(&g_shm_lock);
        return (int32_t)st;
    }
    if (task != g_shm[idx].owner && !g_shm[idx].published) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EACCES;
    }
    out->version = KZ_SHM_STAT_VER;
    out->width = g_shm[idx].width;
    out->height = g_shm[idx].height;
    out->stride = g_shm[idx].stride;
    out->format = g_shm[idx].format;
    out->size_bytes = (uint64_t)g_shm[idx].stride * g_shm[idx].height * 4u;
    out->generation = g_shm[idx].generation;
    out->owner = (uint32_t)g_shm[idx].owner;
    out->rights = KZ_SHM_RIGHT_READ;
    if (task == g_shm[idx].owner) out->rights |= KZ_SHM_RIGHT_WRITE;
    out->state =
        g_shm[idx].published ? KZ_SHM_STATE_PUBLISHED : KZ_SHM_STATE_ACTIVE;
    out->npages = g_shm[idx].npages;
    spinlock_unlock(&g_shm_lock);
    return KZ_OK;
}

int32_t kz_shm_acquire(int32_t task, uint64_t h) {
    uint32_t idx;
    kz_status_t st;

    spinlock_lock(&g_shm_lock);
    st = lookup(h, &idx);
    if (st != KZ_OK) {
        spinlock_unlock(&g_shm_lock);
        return (int32_t)st;
    }
    if (task != g_shm[idx].owner && !g_shm[idx].published) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EACCES;
    }
    if (add_ref(&g_shm[idx], task) != 0) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EBUSY;
    }
    spinlock_unlock(&g_shm_lock);
    return KZ_OK;
}

int32_t kz_shm_release(int32_t task, uint64_t h) {
    uint32_t idx;
    kz_status_t st;

    spinlock_lock(&g_shm_lock);
    st = lookup_unmap(h, &idx);
    if (st != KZ_OK) {
        spinlock_unlock(&g_shm_lock);
        return (int32_t)st;
    }
    if (!has_ref(&g_shm[idx], task)) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EINVAL;
    }
    del_ref(&g_shm[idx], task);
    maybe_free(&g_shm[idx]);
    spinlock_unlock(&g_shm_lock);
    return KZ_OK;
}

int32_t kz_shm_consume(int32_t task, uint64_t h, uint64_t **pages_out,
                       uint32_t *npages_out, uint32_t *stride_out,
                       uint32_t *width_out, uint32_t *height_out,
                       uint32_t *format_out) {
    uint32_t idx;
    kz_status_t st;

    if (!pages_out || !npages_out || !stride_out || !width_out ||
        !height_out || !format_out)
        return KZ_EINVAL;
    spinlock_lock(&g_shm_lock);
    st = lookup(h, &idx);
    if (st != KZ_OK) {
        spinlock_unlock(&g_shm_lock);
        return (int32_t)st;
    }
    if (task != g_shm[idx].owner && !g_shm[idx].published) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EACCES;
    }
    if (add_ref(&g_shm[idx], task) != 0) {
        spinlock_unlock(&g_shm_lock);
        return KZ_EBUSY;
    }
    *pages_out = (uint64_t *)g_shm[idx].pages;
    *npages_out = g_shm[idx].npages;
    *stride_out = g_shm[idx].stride;
    *width_out = g_shm[idx].width;
    *height_out = g_shm[idx].height;
    *format_out = g_shm[idx].format;
    spinlock_unlock(&g_shm_lock);
    return KZ_OK;
}
