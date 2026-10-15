// R4 bridge tests: REAL kz_shm.c + REAL kz_adapter.c vs mock PMM/paging
// (RAM pool) and mock GHAL (RAM surfaces + controllable fences).
// Proves SHM content reaches the backend byte-exact, and that upload,
// present, fence completion, and release stay distinct stages.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kz_adapter.h"
#include "kz_shm.h"
#include "kz_surface.h"
#include "pmm.h"
#include "paging.h"
#include "ghal.h"

static unsigned failures;
static void check(int ok, const char *label) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", label);
    }
}

// --- kmalloc ---
static unsigned live_heap;
void *kmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (p) live_heap++;
    return p;
}
void kfree(void *p) {
    if (p) {
        live_heap--;
        free(p);
    }
}

// --- PMM pool ---
#define POOL_PAGES 16384
static uint8_t pool[POOL_PAGES * 4096];
static unsigned pool_next;
static unsigned live_pages;
uint64_t hhdm_offset;

phys_addr_t pmm_alloc_page(void) {
    if (pool_next >= POOL_PAGES) return PHYS_NULL;
    live_pages++;
    return (phys_addr_t)(0x1000u * (++pool_next));
}
void pmm_free_page(phys_addr_t a) {
    (void)a;
    if (live_pages > 0) live_pages--;
}

// --- paging record ---
#define MAX_MAPS 8192
static struct {
    uint64_t pml4, va, pa, flags;
    int used;
} mtab[MAX_MAPS];

int vmm_map_page_into(uint64_t va, uint64_t pa, uint64_t flags,
                       phys_addr_t pml4) {
    unsigned i;
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

// --- GHAL mock with RAM surfaces (strict: rejects OOB like sw backend) ---
struct ghal_surface {
    uint32_t w, h;
    ghal_format_t fmt;
    uint32_t *pixels;
};
static unsigned live_surfaces;
static int fail_create;
static int fail_present;
static uint64_t mock_fence;
static uint64_t fence_done;
static unsigned upload_calls;
static unsigned upload_rejected;
static ghal_surface_t *last_surface;

ghal_surface_t *ghal_surface_create(uint32_t w, uint32_t h, ghal_format_t f) {
    ghal_surface_t *s;
    if (fail_create) return NULL;
    if (w == 0 || h == 0) return NULL;
    s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->pixels = calloc((size_t)w * h, 4);
    if (!s->pixels) {
        free(s);
        return NULL;
    }
    s->w = w;
    s->h = h;
    s->fmt = f;
    live_surfaces++;
    last_surface = s;
    return s;
}
void ghal_surface_destroy(ghal_surface_t *s) {
    if (s) {
        if (s == last_surface) last_surface = NULL;
        live_surfaces--;
        free(s->pixels);
        free(s);
    }
}
void ghal_surface_upload(ghal_surface_t *s, const uint32_t *src,
                          uint32_t pitch, ghal_rect_t r) {
    uint32_t y;
    upload_calls++;
    if (!s || !src) {
        upload_rejected++;
        return;
    }
    // Same dual-purpose rect semantics as sw_surface_upload: rect is the
    // source offset AND the destination position.
    if (r.x >= s->w || r.y >= s->h || r.w == 0 || r.h == 0 ||
        r.x + r.w > s->w || r.y + r.h > s->h || r.x >= pitch) {
        upload_rejected++;
        return;
    }
    for (y = 0; y < r.h; y++) {
        memcpy(s->pixels + ((uint64_t)r.y + y) * s->w + r.x,
               src + ((uint64_t)r.y + y) * pitch + r.x, r.w * 4);
    }
}
int ghal_present_checked(ghal_surface_t *s, const ghal_rect_t *r) {
    (void)s;
    (void)r;
    if (fail_present) return -1;
    return 0;
}
uint64_t ghal_present_fence(void) {
    return ++mock_fence;
}
int ghal_fence_pending(uint64_t f) {
    return f > fence_done;
}
static void complete_all(void) {
    fence_done = mock_fence;
}

#define PML4_O 0x1000ULL
#define TASK_O 3
#define TASK_C 7

static uint32_t pattern(unsigned x, unsigned y) {
    return ((x * 37u + y * 91u) & 0xFFu) |
           (((x * 53u + y * 29u) & 0xFFu) << 8) |
           (((x * 17u + y * 71u) & 0xFFu) << 16);
}

static void write_pattern(uint64_t h, uint32_t w, uint32_t hh) {
    uint64_t *pages;
    uint32_t np, st, sw, sh, f;
    unsigned x, y;
    if (kz_shm_consume(TASK_O, h, &pages, &np, &st, &sw, &sh, &f) != KZ_OK) {
        check(0, "consume for pattern");
        return;
    }
    for (y = 0; y < hh; y++) {
        for (x = 0; x < w; x++) {
            uint64_t off = ((uint64_t)y * sw + x) * 4u;
            uint32_t *cell = (uint32_t *)(uintptr_t)((uint64_t)(uintptr_t)pool +
                                                     (pages[off / 4096u] /
                                                          0x1000u -
                                                      1u) *
                                                         4096u +
                                                     off % 4096u);
            *cell = pattern(x, y);
        }
    }
    check(kz_shm_release(TASK_O, h) == KZ_OK, "release after pattern");
}

static int64_t make_shm(uint32_t w, uint32_t hh, uint32_t fmt) {
    int64_t h = kz_shm_create(TASK_O, PML4_O, w, hh, fmt);
    if (h <= 0) return h;
    write_pattern((uint64_t)h, w, hh);
    if (kz_shm_publish(TASK_O, (uint64_t)h, 0, 0, (int32_t)w, (int32_t)hh) !=
        KZ_OK)
        return -99;
    return h;
}

static kz_surface_desc_t adescc(uint32_t w, uint32_t hh, uint32_t fmt) {
    kz_surface_desc_t d;
    d.width = w;
    d.height = hh;
    d.stride_px = w;
    d.format = fmt;
    d.size_bytes = (uint64_t)w * hh * 4u;
    d.usage = KZ_USAGE_CPU_READ | KZ_USAGE_CPU_WRITE;
    d.id = 0;
    d.generation = 0;
    d.owner = 0;
    return d;
}

static kz_handle_t make_adapter(uint32_t w, uint32_t hh, uint32_t fmt) {
    kz_surface_desc_t d = adescc(w, hh, fmt);
    kz_handle_t a = 0;
    if (kz_adapter_create(&d, &a) != KZ_OK) return 0;
    if (kz_adapter_acquire(a) != KZ_OK) return 0;
    return a;
}

static void destroy_shm(int64_t h) {
    uint64_t va = kz_shm_slot_va(kz_shm_idx((uint64_t)h));
    kz_shm_unmap(TASK_O, PML4_O, va);
    kz_shm_destroy(TASK_O, (uint64_t)h);
}

static int verify_backend(uint32_t w, uint32_t hh) {
    unsigned x, y;
    if (!last_surface) return 0;
    if (last_surface->w != w || last_surface->h != hh) return 0;
    for (y = 0; y < hh; y++) {
        for (x = 0; x < w; x++) {
            if (last_surface->pixels[y * w + x] != pattern(x, y)) {
                printf("  pixel (%u,%u): got 0x%08X want 0x%08X\n", x, y,
                       last_surface->pixels[y * w + x], pattern(x, y));
                return 0;
            }
        }
    }
    return 1;
}

static void t_full_upload(void) {
    int64_t h = make_shm(64, 48, KZ_FMT_XRGB8888);
    kz_handle_t a = make_adapter(64, 48, KZ_FMT_XRGB8888);
    kz_rect_t r = {0, 0, 64, 48};
    uint32_t state = 0;
    check(h > 0 && a != 0, "bridge setup");
    upload_calls = 0;
    upload_rejected = 0;
    check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)h, &r) == KZ_OK,
          "bridge full upload");
    check(upload_calls > 1, "multi-chunk across pages");
    check(upload_rejected == 0, "no backend rejection");
    check(verify_backend(64, 48), "backend byte-exact");
    check(kz_adapter_get(a, NULL, &state) == KZ_OK && state == KZ_SURF_READY,
          "READY after upload (not presented)");
    check(kz_adapter_present(a, NULL) == KZ_OK, "present submitted");
    check(kz_adapter_get(a, NULL, &state) == KZ_OK &&
              state == KZ_SURF_IN_USE,
          "IN_USE while fence pending");
    complete_all();
    check(kz_adapter_poll(a) == KZ_OK, "poll after completion");
    check(kz_adapter_destroy(a) == KZ_OK, "adapter destroy");
    destroy_shm(h);
}

static void t_wide_stride(void) {
    // 2048x2: a row is 8192 B = 2 pages (wide-row chunk path).
    int64_t h = make_shm(2048, 2, KZ_FMT_XRGB8888);
    kz_handle_t a = make_adapter(2048, 2, KZ_FMT_XRGB8888);
    kz_rect_t r = {0, 0, 2048, 2};
    check(h > 0 && a != 0, "wide setup");
    upload_rejected = 0;
    check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)h, &r) == KZ_OK,
          "wide upload");
    check(upload_rejected == 0, "wide no rejection");
    check(verify_backend(2048, 2), "wide byte-exact");
    check(kz_adapter_destroy(a) == KZ_OK, "wide adapter destroy");
    destroy_shm(h);
    // 100x10: 400 B rows split across pages (odd-stride path).
    h = make_shm(100, 10, KZ_FMT_XRGB8888);
    a = make_adapter(100, 10, KZ_FMT_XRGB8888);
    r.x = 0;
    r.y = 0;
    r.w = 100;
    r.h = 10;
    check(h > 0 && a != 0, "odd setup");
    upload_rejected = 0;
    check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)h, &r) == KZ_OK,
          "odd-stride upload");
    check(upload_rejected == 0, "odd no rejection");
    check(verify_backend(100, 10), "odd byte-exact");
    check(kz_adapter_destroy(a) == KZ_OK, "odd adapter destroy");
    destroy_shm(h);
}

static void t_subrect(void) {
    int64_t h = make_shm(64, 48, KZ_FMT_XRGB8888);
    kz_handle_t a = make_adapter(128, 96, KZ_FMT_XRGB8888);
    kz_rect_t r = {8, 8, 16, 16};
    unsigned x, y;
    int ok = 1;
    check(h > 0 && a != 0, "subrect setup");
    upload_rejected = 0;
    check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)h, &r) == KZ_OK,
          "subrect upload into larger surface");
    check(upload_rejected == 0, "subrect no rejection");
    for (y = 0; y < 16 && ok; y++)
        for (x = 0; x < 16 && ok; x++)
            if (last_surface->pixels[(y + 8) * 128 + (x + 8)] !=
                pattern(x + 8, y + 8))
                ok = 0;
    check(ok, "subrect lands at surface position");
    check(last_surface->pixels[0] == 0, "outside subrect untouched");
    check(kz_adapter_destroy(a) == KZ_OK, "subrect destroy");
    destroy_shm(h);
}

static void t_failures(void) {
    int64_t h = make_shm(32, 32, KZ_FMT_XRGB8888);
    int64_t ha = make_shm(16, 16, KZ_FMT_ARGB8888);
    kz_handle_t a = make_adapter(32, 32, KZ_FMT_XRGB8888);
    kz_handle_t raw;
    kz_surface_desc_t d = adescc(32, 32, KZ_FMT_XRGB8888);
    kz_rect_t r = {0, 0, 32, 32};
    kz_rect_t oob = {16, 16, 32, 32};
    uint32_t state = 0;
    check(h > 0 && ha > 0 && a != 0, "failure setup");
    // adapter not acquired
    check(kz_adapter_create(&d, &raw) == KZ_OK, "raw adapter");
    check(kz_adapter_upload_shm(TASK_C, raw, (uint64_t)h, &r) == KZ_ESTATE,
          "upload before acquire");
    check(kz_adapter_destroy(raw) == KZ_OK, "raw cleanup");
    // stale shm, adapter untouched
    check(kz_adapter_upload_shm(TASK_C, a, kz_shm_handle(0xDEADu, 0), &r) ==
              KZ_ESTALE,
          "stale shm handle");
    check(kz_adapter_get(a, NULL, &state) == KZ_OK &&
              state == KZ_SURF_WRITABLE,
          "adapter still writable after stale");
    // unpublished for a stranger
    {
        int64_t hu = kz_shm_create(TASK_O, PML4_O, 8, 8, KZ_FMT_XRGB8888);
        uint64_t va;
        check(hu > 0, "unpub setup");
        check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)hu, &r) == KZ_EACCES,
              "unpublished refused");
        va = kz_shm_slot_va(kz_shm_idx((uint64_t)hu));
        kz_shm_unmap(TASK_O, PML4_O, va);
        kz_shm_destroy(TASK_O, (uint64_t)hu);
    }
    // rect outside shm dims
    check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)h, &oob) == KZ_EINVAL,
          "oob rect refused");
    // format mismatch
    check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)ha, &r) == KZ_ENOSUP,
          "argb->xrgb refused, never converted");
    // rect outside adapter dims
    {
        kz_handle_t small = make_adapter(8, 8, KZ_FMT_XRGB8888);
        check(kz_adapter_upload_shm(TASK_C, small, (uint64_t)h, &r) ==
                  KZ_EINVAL,
              "rect outside adapter refused");
        check(kz_adapter_destroy(small) == KZ_OK, "small cleanup");
    }
    // null rect
    check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)h, NULL) == KZ_EINVAL,
          "null rect");
    // present failure propagates, upload intact
    fail_present = 1;
    check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)h, &r) == KZ_OK,
          "upload for present-fail");
    check(kz_adapter_present(a, NULL) == KZ_EBUSY, "present fail -> EBUSY");
    fail_present = 0;
    check(kz_adapter_get(a, NULL, &state) == KZ_OK && state == KZ_SURF_READY,
          "still ready after failed present");
    // timeout is not completion: no auto-release
    check(kz_adapter_present(a, NULL) == KZ_OK, "present accepted");
    check(kz_adapter_poll(a) == KZ_EBUSY, "pending poll -> EBUSY");
    check(kz_adapter_get(a, NULL, &state) == KZ_OK &&
              state == KZ_SURF_IN_USE,
          "in use until observed complete");
    complete_all();
    check(kz_adapter_poll(a) == KZ_OK, "late completion retires");
    check(kz_adapter_destroy(a) == KZ_OK, "failure adapter destroy");
    destroy_shm(h);
    destroy_shm(ha);
}

static void t_replace_bridge(void) {
    int64_t h1 = make_shm(32, 32, KZ_FMT_XRGB8888);
    int64_t h2;
    kz_handle_t a = make_adapter(32, 32, KZ_FMT_XRGB8888);
    kz_rect_t r = {0, 0, 32, 32};
    check(h1 > 0 && a != 0, "replace setup");
    check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)h1, &r) == KZ_OK,
          "upload h1");
    h2 = make_shm(32, 32, KZ_FMT_XRGB8888);
    check(h2 > 0 && h2 != h1, "h2 new identity");
    // h2 carries inverted content so the backend comparison is meaningful
    {
        uint64_t *pages;
        uint32_t np, st, sw, sh, f;
        unsigned x, y;
        check(kz_shm_consume(TASK_O, (uint64_t)h2, &pages, &np, &st, &sw,
                             &sh, &f) == KZ_OK,
              "consume h2 for invert");
        for (y = 0; y < 32; y++) {
            for (x = 0; x < 32; x++) {
                uint64_t off = ((uint64_t)y * sw + x) * 4u;
                uint32_t *cell =
                    (uint32_t *)(uintptr_t)((uint64_t)(uintptr_t)pool +
                                            (pages[off / 4096u] / 0x1000u -
                                             1u) *
                                                4096u +
                                            off % 4096u);
                *cell = ~pattern(x, y);
            }
        }
        check(kz_shm_release(TASK_O, (uint64_t)h2) == KZ_OK, "release h2");
    }
    // full cycle between uploads: present, complete, release, re-acquire
    check(kz_adapter_present(a, NULL) == KZ_OK, "present h1");
    complete_all();
    check(kz_adapter_poll(a) == KZ_OK, "release h1 frame");
    check(kz_adapter_acquire(a) == KZ_OK, "re-acquire for h2");
    check(kz_adapter_upload_shm(TASK_C, a, (uint64_t)h2, &r) == KZ_OK,
          "upload h2 into same adapter");
    {
        unsigned x, y;
        int ok = 1;
        for (y = 0; y < 32 && ok; y++)
            for (x = 0; x < 32 && ok; x++)
                if (last_surface->pixels[y * 32 + x] != ~pattern(x, y))
                    ok = 0;
        check(ok, "h2 content wins");
    }
    check(kz_shm_destroy(TASK_O, (uint64_t)h1) == KZ_OK, "retire h1");
    {
        // retired handle rejected on a fresh (WRITABLE) adapter: adapter
        // state is validated before the remote handle by design.
        kz_handle_t fresh = make_adapter(32, 32, KZ_FMT_XRGB8888);
        check(kz_adapter_upload_shm(TASK_C, fresh, (uint64_t)h1, &r) ==
                  KZ_ESTALE,
              "retired h1 rejected");
        check(kz_adapter_destroy(fresh) == KZ_OK, "fresh cleanup");
    }
    check(kz_adapter_destroy(a) == KZ_OK, "replace adapter destroy");
    destroy_shm(h1);
    destroy_shm(h2);
}

int main(void) {
    unsigned heap_base = live_heap;
    unsigned pages_base = live_pages;
    hhdm_offset = (uint64_t)(uintptr_t)pool - 0x1000u;
    t_full_upload();
    t_wide_stride();
    t_subrect();
    t_failures();
    t_replace_bridge();
    check(live_heap == heap_base, "no heap leaks");
    check(live_pages == pages_base, "no page leaks");
    check(live_surfaces == 0, "no backend surfaces leaked");
    printf("kz-bridge failures=%u\n", failures);
    return failures ? 1 : 0;
}
