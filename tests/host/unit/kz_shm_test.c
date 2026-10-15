// R3 shared-surface tests: REAL kernel/mm/kz_shm.c vs a RAM model of
// PMM (bump pool) and paging (recorded table). No hardware, no QEMU.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kz_shm.h"
#include "kz_surface.h"
#include "pmm.h"
#include "paging.h"

static unsigned failures;
static void check(int ok, const char *label) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", label);
    }
}

// --- kmalloc mock ---
static int fail_kmalloc;
static unsigned live_heap;
void *kmalloc(size_t n) {
    void *p;
    if (fail_kmalloc) return NULL;
    p = malloc(n ? n : 1);
    if (p) live_heap++;
    return p;
}
void kfree(void *p) {
    if (p) {
        live_heap--;
        free(p);
    }
}

// --- PMM mock: bump pool, phys = 0x1000*(index+1) ---
#define POOL_PAGES 8192
static uint8_t pool[POOL_PAGES * 4096];
static unsigned pool_next;
static unsigned live_pages;
static int fail_pmm_at = -1;
uint64_t hhdm_offset;

phys_addr_t pmm_alloc_page(void) {
    if (fail_pmm_at >= 0 && (int)pool_next >= fail_pmm_at) return PHYS_NULL;
    if (pool_next >= POOL_PAGES) return PHYS_NULL;
    live_pages++;
    return (phys_addr_t)(0x1000u * (++pool_next));
}
void pmm_free_page(phys_addr_t a) {
    (void)a;
    if (live_pages > 0) live_pages--;
}

// --- paging mock: recorded (pml4, va) -> (pa, flags) ---
#define MAX_MAPS 4096
static struct {
    uint64_t pml4, va, pa, flags;
    int used;
} mtab[MAX_MAPS];
static int fail_map_at = -1;
static int map_calls;

int vmm_map_page_into(uint64_t va, uint64_t pa, uint64_t flags,
                       phys_addr_t pml4) {
    unsigned i;
    if (fail_map_at >= 0 && map_calls >= fail_map_at) return 0;
    map_calls++;
    for (i = 0; i < MAX_MAPS; i++) {
        if (!mtab[i].used) {
            mtab[i].used = 1;
            mtab[i].pml4 = pml4;
            mtab[i].va = va;
            mtab[i].pa = pa;
            mtab[i].flags = flags;
            return 1;
        }
    }
    return 0;
}
phys_addr_t vmm_unmap_page_from(uint64_t va, phys_addr_t pml4) {
    unsigned i;
    for (i = 0; i < MAX_MAPS; i++) {
        if (mtab[i].used && mtab[i].pml4 == pml4 && mtab[i].va == va) {
            phys_addr_t pa = mtab[i].pa;
            mtab[i].used = 0;
            return pa;
        }
    }
    return PHYS_NULL;
}
void vmm_tlb_shootdown(uint64_t va) {
    (void)va;
}

static uint64_t map_flags(uint64_t pml4, uint64_t va, int *found) {
    unsigned i;
    for (i = 0; i < MAX_MAPS; i++) {
        if (mtab[i].used && mtab[i].pml4 == pml4 && mtab[i].va == va) {
            *found = 1;
            return mtab[i].flags;
        }
    }
    *found = 0;
    return 0;
}

static uint64_t map_pa(uint64_t pml4, uint64_t va, int *found) {
    unsigned i;
    for (i = 0; i < MAX_MAPS; i++) {
        if (mtab[i].used && mtab[i].pml4 == pml4 && mtab[i].va == va) {
            *found = 1;
            return mtab[i].pa;
        }
    }
    *found = 0;
    return 0;
}

#define PML4_A 0xA000ULL
#define PML4_B 0xB000ULL
#define TASK_A 3
#define TASK_B 7

static void t_create_valid(void) {
    int64_t h = kz_shm_create(TASK_A, PML4_A, 64, 32, KZ_FMT_XRGB8888);
    uint64_t va;
    int found;
    uint64_t fl;
    check(h > 0, "create returns positive handle");
    check(kz_shm_idx((uint64_t)h) < KZ_SHM_MAX_RES, "idx in range");
    check(kz_shm_gen((uint64_t)h) != 0, "gen nonzero");
    va = kz_shm_slot_va(kz_shm_idx((uint64_t)h));
    check(va >= KZ_SHM_BASE && va < KZ_SHM_END, "slot va in range");
    fl = map_flags(PML4_A, va, &found);
    check(found, "owner auto-mapped at create");
    check((fl & PAGING_WRITABLE) != 0, "owner mapping writable");
    check((fl & PAGING_USER) != 0, "owner mapping user");
    check((fl & PAGING_NX) != 0, "owner mapping NX");
    // fresh pages are zeroed (no cross-task leak): resolve the first page
    // through the recorded mapping and read pool memory directly.
    {
        int pfound = 0;
        uint64_t pa = map_pa(PML4_A, va, &pfound);
        uint32_t *cell;
        check(pfound, "mapping recorded");
        cell = (uint32_t *)(uintptr_t)((uint64_t)(uintptr_t)pool +
                                       (pa / 0x1000u - 1u) * 4096u);
        check(cell[0] == 0 && cell[1023] == 0, "fresh pages zeroed");
    }
    check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_OK, "owner unmap");
    check(kz_shm_destroy(TASK_A, (uint64_t)h) == KZ_OK, "destroy");
}

static void t_create_invalid(void) {
    check(kz_shm_create(TASK_A, PML4_A, 0, 32, KZ_FMT_XRGB8888) == KZ_EINVAL,
          "zero width");
    check(kz_shm_create(TASK_A, PML4_A, 32, 0, KZ_FMT_XRGB8888) == KZ_EINVAL,
          "zero height");
    check(kz_shm_create(TASK_A, PML4_A, 0xFFFFFFFFu, 0xFFFFFFFFu,
                        KZ_FMT_XRGB8888) == KZ_EINVAL,
          "u32 overflow dims");
    check(kz_shm_create(TASK_A, PML4_A, 64, 32, 99) == KZ_EINVAL, "bad format");
    check(kz_shm_create(-1, PML4_A, 64, 32, KZ_FMT_XRGB8888) == KZ_EINVAL,
          "bad task");
    check(kz_shm_create(TASK_A, PHYS_NULL, 64, 32, KZ_FMT_XRGB8888) ==
              KZ_EINVAL,
          "null pml4");
    // 8 MiB cap: 2048x1024x4 = 8 MiB ok; one row more exceeds
    {
        int64_t big = kz_shm_create(TASK_A, PML4_A, 2048, 1024,
                                    KZ_FMT_XRGB8888);
        check(big > 0, "8 MiB max ok");
        if (big > 0) {
            uint64_t va = kz_shm_slot_va(kz_shm_idx((uint64_t)big));
            kz_shm_unmap(TASK_A, PML4_A, va);
            kz_shm_destroy(TASK_A, (uint64_t)big);
        }
    }
    check(kz_shm_create(TASK_A, PML4_A, 2048, 1025, KZ_FMT_XRGB8888) ==
              KZ_EINVAL,
          "over 8 MiB refused");
}

static void t_table_full(void) {
    int64_t held[KZ_SHM_MAX_RES];
    unsigned n = 0;
    int64_t h;
    int i;
    for (i = 0; i < KZ_SHM_MAX_RES; i++) {
        h = kz_shm_create(TASK_A, PML4_A, 8, 8, KZ_FMT_XRGB8888);
        if (h < 0) break;
        held[n++] = h;
    }
    check(n == KZ_SHM_MAX_RES, "table fills");
    check(kz_shm_create(TASK_A, PML4_A, 8, 8, KZ_FMT_XRGB8888) == KZ_EBUSY,
          "table full EBUSY");
    for (i = 0; i < (int)n; i++) {
        uint64_t va = kz_shm_slot_va(kz_shm_idx((uint64_t)held[i]));
        check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_OK, "drain unmap");
        check(kz_shm_destroy(TASK_A, (uint64_t)held[i]) == KZ_OK,
              "drain destroy");
    }
}

static void t_rights(void) {
    int64_t h = kz_shm_create(TASK_A, PML4_A, 32, 32, KZ_FMT_XRGB8888);
    uint64_t va;
    int found;
    uint64_t fl;
    check(h > 0, "rights setup");
    // unpublished: stranger gets nothing
    check(kz_shm_map(TASK_B, PML4_B, (uint64_t)h, 0) == 0,
          "unpublished invisible");
    check(kz_shm_publish(TASK_A, (uint64_t)h, 0, 0, 32, 32) == KZ_OK,
          "owner publish");
    va = kz_shm_map(TASK_B, PML4_B, (uint64_t)h, 0);
    check(va == kz_shm_slot_va(kz_shm_idx((uint64_t)h)), "consumer same VA");
    fl = map_flags(PML4_B, va, &found);
    check(found && (fl & PAGING_WRITABLE) == 0, "consumer read-only");
    check(kz_shm_unmap(TASK_B, PML4_B, va) == KZ_OK, "consumer unmap");
    check(kz_shm_map(TASK_B, PML4_B, (uint64_t)h, KZ_SHM_MAP_WRITE) == 0,
          "consumer write refused");
    check(kz_shm_publish(TASK_B, (uint64_t)h, 0, 0, 1, 1) == KZ_EACCES,
          "consumer publish refused");
    check(kz_shm_destroy(TASK_B, (uint64_t)h) == KZ_EACCES,
          "consumer destroy refused");
    // owner write mapping
    va = kz_shm_map(TASK_A, PML4_A, (uint64_t)h, KZ_SHM_MAP_WRITE);
    check(va != 0, "owner write map");
    fl = map_flags(PML4_A, va, &found);
    check(found && (fl & PAGING_WRITABLE) != 0, "owner write granted");
    // idempotent re-map returns the same VA without new work
    {
        int calls_before = map_calls;
        uint64_t va2 = kz_shm_map(TASK_A, PML4_A, (uint64_t)h, 0);
        check(va2 == va && map_calls == calls_before, "re-map idempotent");
    }
    check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_OK, "owner unmap");
    check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_EINVAL, "unmap twice");
    check(kz_shm_destroy(TASK_A, (uint64_t)h) == KZ_OK, "owner destroy");
}

static void t_stale(void) {
    int64_t h = kz_shm_create(TASK_A, PML4_A, 16, 16, KZ_FMT_XRGB8888);
    uint64_t va;
    int64_t h2;
    check(h > 0, "stale setup");
    va = kz_shm_slot_va(kz_shm_idx((uint64_t)h));
    check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_OK, "stale unmap");
    check(kz_shm_destroy(TASK_A, (uint64_t)h) == KZ_OK, "stale destroy");
    check(kz_shm_map(TASK_A, PML4_A, (uint64_t)h, 0) == 0, "use after free");
    check(kz_shm_destroy(TASK_A, (uint64_t)h) == KZ_ESTALE, "double destroy");
    check(kz_shm_publish(TASK_A, (uint64_t)h, 0, 0, 1, 1) == KZ_ESTALE,
          "publish after free");
    h2 = kz_shm_create(TASK_A, PML4_A, 16, 16, KZ_FMT_XRGB8888);
    check(h2 > 0 && h2 != h, "reuse bumps generation");
    check(kz_shm_map(TASK_A, PML4_A, (uint64_t)h, 0) == 0, "old gen dead");
    va = kz_shm_slot_va(kz_shm_idx((uint64_t)h2));
    check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_OK, "stale cleanup unmap");
    check(kz_shm_destroy(TASK_A, (uint64_t)h2) == KZ_OK, "stale cleanup");
    check(kz_shm_map(TASK_A, PML4_A, 0xFFFFFFFFFFFFFFFFull, 0) == 0,
          "wild handle");
}

static void t_publish_bounds(void) {
    int64_t h = kz_shm_create(TASK_A, PML4_A, 32, 32, KZ_FMT_XRGB8888);
    uint64_t va;
    check(h > 0, "pub setup");
    check(kz_shm_publish(TASK_A, (uint64_t)h, 0, 0, 33, 32) == KZ_EINVAL,
          "damage overflow x");
    check(kz_shm_publish(TASK_A, (uint64_t)h, -1, 0, 4, 4) == KZ_EINVAL,
          "damage negative");
    check(kz_shm_publish(TASK_A, (uint64_t)h, 0, 0, -2, 4) == KZ_EINVAL,
          "damage negative size");
    check(kz_shm_publish(TASK_A, (uint64_t)h, 0, 0, 32, 32) == KZ_OK,
          "exact bounds ok");
    va = kz_shm_slot_va(kz_shm_idx((uint64_t)h));
    check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_OK, "pub cleanup unmap");
    check(kz_shm_destroy(TASK_A, (uint64_t)h) == KZ_OK, "pub cleanup");
}

static void t_unmap_bounds(void) {
    check(kz_shm_unmap(TASK_A, PML4_A, 0x1000) == KZ_EINVAL, "non-shm va");
    check(kz_shm_unmap(TASK_A, PML4_A, KZ_SHM_BASE + 1) == KZ_EINVAL,
          "unaligned va");
    check(kz_shm_unmap(TASK_A, PML4_A, KZ_SHM_END) == KZ_EINVAL, "end va");
    check(kz_shm_unmap(TASK_A, PML4_A, KZ_SHM_BASE) == KZ_EINVAL,
          "unmapped slot");
}

static void t_lazy_retire(void) {
    int64_t h = kz_shm_create(TASK_A, PML4_A, 32, 32, KZ_FMT_XRGB8888);
    unsigned pages_before;
    uint64_t va;
    check(h > 0, "retire setup");
    pages_before = live_pages;
    check(kz_shm_publish(TASK_A, (uint64_t)h, 0, 0, 32, 32) == KZ_OK,
          "retire publish");
    va = kz_shm_map(TASK_B, PML4_B, (uint64_t)h, 0);
    check(va != 0, "retire consumer map");
    check(kz_shm_destroy(TASK_A, (uint64_t)h) == KZ_OK, "destroy in use");
    check(live_pages == pages_before, "pages retained for consumer");
    check(kz_shm_map(TASK_B, PML4_B, (uint64_t)h, 0) == 0,
          "no new maps after destroy");
    check(kz_shm_unmap(TASK_B, PML4_B, va) == KZ_OK, "consumer unmap");
    check(live_pages == pages_before, "owner record still retains");
    va = kz_shm_slot_va(kz_shm_idx((uint64_t)h));
    check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_OK,
          "owner releases destroyed mapping");
    check(live_pages == pages_before - 1, "pages freed after last unmap");
}

static void t_exit_cleanup(void) {
    unsigned pages_before = live_pages;
    int64_t h = kz_shm_create(TASK_A, PML4_A, 32, 32, KZ_FMT_XRGB8888);
    uint64_t va;
    check(h > 0, "exit setup");
    check(kz_shm_publish(TASK_A, (uint64_t)h, 0, 0, 32, 32) == KZ_OK,
          "exit publish");
    va = kz_shm_map(TASK_B, PML4_B, (uint64_t)h, 0);
    check(va != 0, "exit consumer map");
    kz_shm_task_exit(TASK_B);
    check(kz_shm_unmap(TASK_B, PML4_B, va) == KZ_EINVAL,
          "dead consumer mapping gone");
    kz_shm_task_exit(TASK_A);
    check(live_pages == pages_before, "owner exit frees all pages");
    check(kz_shm_map(TASK_A, PML4_A, (uint64_t)h, 0) == 0,
          "owner exit kills handle");
}

static void t_partial_init(void) {
    int64_t h;
    fail_pmm_at = 0;
    h = kz_shm_create(TASK_A, PML4_A, 64, 64, KZ_FMT_XRGB8888);
    fail_pmm_at = -1;
    check(h == KZ_ENOMEM, "pmm exhaustion -> ENOMEM");
    h = kz_shm_create(TASK_A, PML4_A, 8, 8, KZ_FMT_XRGB8888);
    check(h > 0, "slot reusable after failed create");
    if (h > 0) {
        uint64_t va = kz_shm_slot_va(kz_shm_idx((uint64_t)h));
        kz_shm_unmap(TASK_A, PML4_A, va);
        kz_shm_destroy(TASK_A, (uint64_t)h);
    }
    fail_kmalloc = 1;
    check(kz_shm_create(TASK_A, PML4_A, 8, 8, KZ_FMT_XRGB8888) == KZ_ENOMEM,
          "kmalloc fail -> ENOMEM");
    fail_kmalloc = 0;
}

static void t_map_fail(void) {
    int64_t h;
    fail_map_at = 0;
    h = kz_shm_create(TASK_A, PML4_A, 16, 16, KZ_FMT_XRGB8888);
    fail_map_at = -1;
    check(h == KZ_ENOMEM, "map failure rolls back create");
    h = kz_shm_create(TASK_A, PML4_A, 16, 16, KZ_FMT_XRGB8888);
    check(h > 0, "recreate after map rollback");
    if (h > 0) {
        uint64_t va = kz_shm_slot_va(kz_shm_idx((uint64_t)h));
        check(kz_shm_publish(TASK_A, (uint64_t)h, 0, 0, 16, 16) == KZ_OK,
              "publish for consumer map-fail");
        fail_map_at = map_calls;
        check(kz_shm_map(TASK_B, PML4_B, (uint64_t)h, 0) == 0,
              "consumer map failure -> 0");
        fail_map_at = -1;
        // no record leaked: consumer has nothing to unmap
        check(kz_shm_unmap(TASK_B, PML4_B, va) == KZ_EINVAL,
              "failed map leaves no record");
        check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_OK, "owner unmap");
        check(kz_shm_destroy(TASK_A, (uint64_t)h) == KZ_OK, "destroy");
    }
}

static void t_stat_acquire_release(void) {
    int64_t h = kz_shm_create(TASK_A, PML4_A, 48, 24, KZ_FMT_ARGB8888);
    kz_shm_stat_t st;
    uint64_t va;
    check(h > 0, "stat setup");
    check(kz_shm_stat(TASK_A, (uint64_t)h, NULL) == KZ_EINVAL, "null out");
    check(kz_shm_stat(TASK_A, (uint64_t)h, &st) == KZ_OK, "owner stat");
    check(st.version == KZ_SHM_STAT_VER, "stat version");
    check(st.width == 48 && st.height == 24, "stat dims authoritative");
    check(st.stride == 48 && st.format == KZ_FMT_ARGB8888, "stat stride/fmt");
    check(st.size_bytes == (uint64_t)48 * 24 * 4, "stat size");
    check(st.generation == kz_shm_gen((uint64_t)h), "stat gen matches");
    check(st.owner == (uint32_t)TASK_A, "stat owner");
    check(st.rights == (KZ_SHM_RIGHT_READ | KZ_SHM_RIGHT_WRITE),
          "owner rights RW");
    check(st.state == KZ_SHM_STATE_ACTIVE, "active before publish");
    check(st.npages == 2, "stat npages (4608 B -> 2 pages)");
    // stranger sees nothing before publish
    check(kz_shm_stat(TASK_B, (uint64_t)h, &st) == KZ_EACCES, "stat gated");
    check(kz_shm_acquire(TASK_B, (uint64_t)h) == KZ_EACCES,
          "acquire unpublished");
    check(kz_shm_publish(TASK_A, (uint64_t)h, 0, 0, 48, 24) == KZ_OK,
          "publish for stat");
    check(kz_shm_stat(TASK_B, (uint64_t)h, &st) == KZ_OK, "consumer stat");
    check(st.state == KZ_SHM_STATE_PUBLISHED, "published visible");
    check(st.rights == KZ_SHM_RIGHT_READ, "consumer rights RO");
    check(st.width == 48, "consumer sees true dims, not message dims");
    // acquire/release counting
    check(kz_shm_acquire(TASK_B, (uint64_t)h) == KZ_OK, "acquire 1");
    check(kz_shm_acquire(TASK_B, (uint64_t)h) == KZ_OK, "acquire 2");
    check(kz_shm_release(TASK_B, (uint64_t)h) == KZ_OK, "release 1");
    check(kz_shm_release(TASK_B, (uint64_t)h) == KZ_OK, "release 2");
    check(kz_shm_release(TASK_B, (uint64_t)h) == KZ_EINVAL,
          "release without ref");
    check(kz_shm_release(TASK_A, (uint64_t)h) == KZ_EINVAL,
          "owner release without acquire");
    check(kz_shm_acquire(TASK_A, (uint64_t)h) == KZ_OK, "owner acquire");
    check(kz_shm_release(TASK_A, (uint64_t)h) == KZ_OK, "owner release");
    // destroy with outstanding acquire: pages retained, release still works
    check(kz_shm_acquire(TASK_B, (uint64_t)h) == KZ_OK, "acquire pre-destroy");
    check(kz_shm_destroy(TASK_A, (uint64_t)h) == KZ_OK, "destroy with ref");
    check(kz_shm_acquire(TASK_B, (uint64_t)h) == KZ_ESTALE,
          "no acquire after destroy");
    check(kz_shm_release(TASK_B, (uint64_t)h) == KZ_OK,
          "release after destroy retires");
    va = kz_shm_slot_va(kz_shm_idx((uint64_t)h));
    check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_OK, "stat cleanup unmap");
    check(kz_shm_stat(TASK_A, (uint64_t)h, &st) == KZ_ESTALE,
          "stat after free");
}

static void t_replacement(void) {
    int64_t h1, h2, h3;
    uint64_t va1, va2;
    kz_shm_stat_t st;
    int i;
    h1 = kz_shm_create(TASK_A, PML4_A, 32, 32, KZ_FMT_XRGB8888);
    check(h1 > 0, "replace setup");
    check(kz_shm_publish(TASK_A, (uint64_t)h1, 0, 0, 32, 32) == KZ_OK,
          "publish h1");
    va1 = kz_shm_map(TASK_B, PML4_B, (uint64_t)h1, 0);
    check(va1 != 0, "consumer maps h1");
    check(kz_shm_acquire(TASK_B, (uint64_t)h1) == KZ_OK, "consumer holds h1");
    // replacement: new identity, new generation
    h2 = kz_shm_create(TASK_A, PML4_A, 64, 16, KZ_FMT_XRGB8888);
    check(h2 > 0 && h2 != h1, "h2 new identity");
    check(kz_shm_publish(TASK_A, (uint64_t)h2, 0, 0, 64, 16) == KZ_OK,
          "publish h2");
    va2 = kz_shm_map(TASK_B, PML4_B, (uint64_t)h2, 0);
    check(va2 != 0 && va2 != va1, "h2 maps at its own slot VA");
    check(kz_shm_stat(TASK_B, (uint64_t)h2, &st) == KZ_OK, "stat h2");
    check(st.width == 64 && st.height == 16, "h2 dims differ from h1");
    // retire old: outstanding consumer mapping keeps working
    check(kz_shm_destroy(TASK_A, (uint64_t)h1) == KZ_OK, "retire h1");
    check(kz_shm_map(TASK_B, PML4_B, (uint64_t)h1, 0) == 0,
          "h1 closed for newcomers");
    check(kz_shm_release(TASK_B, (uint64_t)h1) == KZ_OK, "h1 ref dropped");
    check(kz_shm_unmap(TASK_B, PML4_B, va1) == KZ_OK, "h1 mapping dropped");
    check(kz_shm_unmap(TASK_B, PML4_B, va2) == KZ_OK, "h2 mapping dropped");
    va1 = kz_shm_slot_va(kz_shm_idx((uint64_t)h1));
    va2 = kz_shm_slot_va(kz_shm_idx((uint64_t)h2));
    check(kz_shm_unmap(TASK_A, PML4_A, va1) == KZ_OK, "owner h1 unmap");
    check(kz_shm_unmap(TASK_A, PML4_A, va2) == KZ_OK, "owner h2 unmap");
    check(kz_shm_destroy(TASK_A, (uint64_t)h2) == KZ_OK, "destroy h2");
    // repeated replacement: 3 cycles, generations advance, no leaks
    for (i = 0; i < 3; i++) {
        h3 = kz_shm_create(TASK_A, PML4_A, 16, 16, KZ_FMT_XRGB8888);
        if (h3 <= 0) break;
        kz_shm_publish(TASK_A, (uint64_t)h3, 0, 0, 16, 16);
        va1 = kz_shm_slot_va(kz_shm_idx((uint64_t)h3));
        kz_shm_unmap(TASK_A, PML4_A, va1);
        kz_shm_destroy(TASK_A, (uint64_t)h3);
    }
    check(i == 3, "3 replace cycles");
}

static void t_consume(void) {
    int64_t h = kz_shm_create(TASK_A, PML4_A, 32, 32, KZ_FMT_XRGB8888);
    uint64_t *pages;
    uint32_t np, st, w, hh, f;
    uint64_t va;
    check(h > 0, "consume setup");
    check(kz_shm_consume(TASK_B, (uint64_t)h, &pages, &np, &st, &w, &hh,
                         &f) == KZ_EACCES,
          "consume unpublished");
    check(kz_shm_publish(TASK_A, (uint64_t)h, 0, 0, 32, 32) == KZ_OK,
          "publish for consume");
    check(kz_shm_consume(TASK_B, (uint64_t)h, NULL, &np, &st, &w, &hh,
                         &f) == KZ_EINVAL,
          "consume null out");
    check(kz_shm_consume(TASK_B, (uint64_t)h, &pages, &np, &st, &w, &hh,
                         &f) == KZ_OK,
          "consume ok");
    check(pages != NULL && np == 1 && st == 32 && w == 32 && hh == 32 &&
              f == KZ_FMT_XRGB8888,
          "consume borrows true allocation");
    check(kz_shm_destroy(TASK_A, (uint64_t)h) == KZ_OK, "destroy consumed");
    check(kz_shm_release(TASK_B, (uint64_t)h) == KZ_OK, "release retires");
    va = kz_shm_slot_va(kz_shm_idx((uint64_t)h));
    check(kz_shm_unmap(TASK_A, PML4_A, va) == KZ_OK, "consume cleanup");
}

static void t_protocol(void) {
    kz_shm_msg_t m;
    check(sizeof(kz_shm_msg_t) == 52, "message is fixed 52 bytes");
    check(KZ_SHM_MSG_MAGIC == 0x53484D50u, "magic constant");
    check(KZ_SHM_MSG_VER == 1u, "version 1");
    memset(&m, 0, sizeof(m));
    m.magic = KZ_SHM_MSG_MAGIC;
    m.version = KZ_SHM_MSG_VER;
    m.type = KZ_SHM_MSG_PUBLISH;
    m.gen = 5;
    m.id = 2;
    m.x = 1;
    m.y = 2;
    m.w = 3;
    m.h = 4;
    check(m.magic == KZ_SHM_MSG_MAGIC && m.version == KZ_SHM_MSG_VER,
          "message round-trips");
    check(m.type == KZ_SHM_MSG_PUBLISH && m.gen == 5 && m.id == 2,
          "handle travels as id+gen, never a pointer");
}

int main(void) {
    unsigned heap_base = live_heap;
    unsigned pages_base = live_pages;
    hhdm_offset = (uint64_t)(uintptr_t)pool - 0x1000u;
    t_create_valid();
    t_create_invalid();
    t_table_full();
    t_rights();
    t_stale();
    t_publish_bounds();
    t_unmap_bounds();
    t_lazy_retire();
    t_exit_cleanup();
    t_partial_init();
    t_map_fail();
    t_stat_acquire_release();
    t_replacement();
    t_consume();
    t_protocol();
    check(live_heap == heap_base, "no heap leaks");
    check(live_pages == pages_base, "no page leaks");
    printf("kz-shm failures=%u\n", failures);
    return failures ? 1 : 0;
}
