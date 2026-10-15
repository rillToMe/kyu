// R2 adapter tests: REAL kernel/gfx/kz_adapter.c vs mock GHAL backend.
// No hardware, no IRQ, no scanout. Failures are injected via globals.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kz_adapter.h"
#include "kz_surface.h"
#include "ghal.h"

static unsigned failures;
static void check(int ok, const char *label) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", label);
    }
}

// --- kmalloc/kfree mock (heap.h) ---
static int fail_alloc;
static unsigned live_heap;
void *kmalloc(size_t n) {
    void *p;
    if (fail_alloc) return NULL;
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

// R4: kz_adapter.c also references the SHM store (bridge path). This test
// never exercises the bridge (covered by test-kz-bridge); stub the two
// store entry points + HHDM base so the TU links.
uint64_t hhdm_offset;
int32_t kz_shm_consume(int32_t task, uint64_t h, uint64_t **p, uint32_t *n,
                       uint32_t *s, uint32_t *w, uint32_t *hh, uint32_t *f) {
    (void)task;
    (void)h;
    (void)p;
    (void)n;
    (void)s;
    (void)w;
    (void)hh;
    (void)f;
    return KZ_ESTALE;
}
int32_t kz_shm_release(int32_t task, uint64_t h) {
    (void)task;
    (void)h;
    return KZ_ESTALE;
}

// --- GHAL mock backend ---
struct ghal_surface {
    uint32_t w, h;
    ghal_format_t fmt;
    uint32_t tag;
};
static unsigned live_surfaces;
static int fail_create;
static int fail_present;
static uint64_t mock_fence;
static uint64_t fence_done;
static ghal_rect_t last_upload_rect;
static const uint32_t *last_upload_src;
static unsigned upload_count;
static ghal_rect_t last_present_rect;
static int last_present_null;
static unsigned present_count;

ghal_surface_t *ghal_surface_create(uint32_t w, uint32_t h, ghal_format_t f) {
    ghal_surface_t *s;
    if (fail_create) return NULL;
    s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    s->fmt = f;
    s->tag = 0xADAE55u;
    live_surfaces++;
    return s;
}
void ghal_surface_destroy(ghal_surface_t *s) {
    if (s) {
        live_surfaces--;
        free(s);
    }
}
void ghal_surface_upload(ghal_surface_t *s, const uint32_t *src,
                          uint32_t pitch, ghal_rect_t r) {
    (void)s;
    last_upload_src = src;
    last_upload_rect = r;
    (void)pitch;
    upload_count++;
}
int ghal_present_checked(ghal_surface_t *s, const ghal_rect_t *r) {
    (void)s;
    if (fail_present) return -1;
    present_count++;
    if (!r) {
        last_present_null = 1;
    } else {
        last_present_null = 0;
        last_present_rect = *r;
    }
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

static kz_surface_desc_t good_desc(uint32_t w, uint32_t h) {
    kz_surface_desc_t d;
    d.width = w;
    d.height = h;
    d.stride_px = w;
    d.format = KZ_FMT_XRGB8888;
    d.size_bytes = (uint64_t)w * h * 4;
    d.usage = KZ_USAGE_CPU_READ | KZ_USAGE_CPU_WRITE;
    d.id = 1;
    d.generation = 0;
    d.owner = 0;
    return d;
}

static uint32_t pixels[64 * 64];

static kz_handle_t make_ready(uint32_t w, uint32_t h) {
    kz_surface_desc_t d = good_desc(w, h);
    kz_handle_t hh = 0;
    kz_rect_t r = {0, 0, (int32_t)w, (int32_t)h};
    check(kz_adapter_create(&d, &hh) == KZ_OK, "setup create");
    check(kz_adapter_acquire(hh) == KZ_OK, "setup acquire");
    check(kz_adapter_upload(hh, pixels, w, &r) == KZ_OK, "setup upload");
    return hh;
}

// 1. valid creation
static void t_create_valid(void) {
    kz_surface_desc_t d = good_desc(16, 16);
    kz_handle_t h = 0;
    kz_surface_desc_t got;
    uint32_t state = 99;
    check(kz_adapter_create(&d, &h) == KZ_OK, "create valid");
    check(h != 0, "handle nonzero");
    check(kz_adapter_get(h, &got, &state) == KZ_OK, "get valid");
    check(got.width == 16 && got.height == 16, "desc preserved");
    check(got.format == KZ_FMT_XRGB8888, "format preserved");
    check(state == KZ_SURF_CREATED, "state created");
    check(kz_adapter_destroy(h) == KZ_OK, "destroy idle");
}

// 2. invalid dimensions / overflow
static void t_create_dims(void) {
    kz_surface_desc_t d;
    kz_handle_t h = 0;
    d = good_desc(0, 16);
    check(kz_adapter_create(&d, &h) == KZ_EINVAL, "zero width");
    d = good_desc(16, 0);
    check(kz_adapter_create(&d, &h) == KZ_EINVAL, "zero height");
    d = good_desc(0xFFFFFFFFu, 0xFFFFFFFFu);
    check(kz_adapter_create(&d, &h) == KZ_EINVAL, "u32 overflow dims");
    check(kz_adapter_create(NULL, &h) == KZ_EINVAL, "null desc");
    d = good_desc(16, 16);
    check(kz_adapter_create(&d, NULL) == KZ_EINVAL, "null out");
}

// 3. invalid stride / undersized
static void t_create_stride(void) {
    kz_surface_desc_t d;
    kz_handle_t h = 0;
    d = good_desc(16, 16);
    d.stride_px = 15;
    check(kz_adapter_create(&d, &h) == KZ_EINVAL, "short stride");
    d = good_desc(16, 16);
    d.size_bytes = (uint64_t)16 * 16 * 4 - 4;
    check(kz_adapter_create(&d, &h) == KZ_EINVAL, "short backing");
    d = good_desc(16, 16);
    d.usage = 0;
    check(kz_adapter_create(&d, &h) == KZ_EINVAL, "zero usage");
}

// 4. unsupported format
static void t_create_format(void) {
    kz_surface_desc_t d = good_desc(16, 16);
    kz_handle_t h = 0;
    d.format = 99;
    check(kz_adapter_create(&d, &h) == KZ_EINVAL, "bad format");
    d = good_desc(8, 8);
    d.format = KZ_FMT_ARGB8888;
    check(kz_adapter_create(&d, &h) == KZ_OK, "argb accepted");
    check(kz_adapter_destroy(h) == KZ_OK, "argb destroyed");
}

// 5. invalid lifecycle transitions
static void t_lifecycle(void) {
    kz_handle_t h = make_ready(16, 16);
    kz_rect_t r = {0, 0, 16, 16};
    kz_handle_t h2;
    kz_surface_desc_t d = good_desc(8, 8);
    check(kz_adapter_acquire(h) == KZ_ESTATE, "acquire when ready rejected");
    check(kz_adapter_upload(h, pixels, 16, &r) == KZ_ESTATE,
          "upload when ready rejected");
    check(kz_adapter_create(&d, &h2) == KZ_OK, "second create");
    check(kz_adapter_upload(h2, pixels, 8, &r) == KZ_ESTATE,
          "upload before acquire rejected");
    check(kz_adapter_present(h2, NULL) == KZ_ESTATE,
          "present before upload rejected");
    check(kz_adapter_poll(h2) == KZ_ESTATE, "poll non-inflight rejected");
    check(kz_adapter_acquire(h2) == KZ_OK, "acquire ok");
    check(kz_adapter_acquire(h2) == KZ_ESTATE, "double acquire rejected");
    check(kz_adapter_destroy(h2) == KZ_OK, "cleanup h2");
    check(kz_adapter_destroy(h) == KZ_OK, "cleanup h");
}

// 6. stale handle / generation
static void t_stale(void) {
    kz_surface_desc_t d = good_desc(16, 16);
    kz_handle_t h = 0;
    kz_handle_t h2 = 0;
    check(kz_adapter_create(&d, &h) == KZ_OK, "stale setup");
    check(kz_adapter_destroy(h) == KZ_OK, "stale destroy");
    check(kz_adapter_acquire(h) == KZ_ESTALE, "use after destroy stale");
    check(kz_adapter_get(h, NULL, NULL) == KZ_ESTALE, "get after destroy");
    check(kz_adapter_create(&d, &h2) == KZ_OK, "slot reused");
    check(h2 != h, "generation bumped handle");
    check(kz_adapter_acquire(h) == KZ_ESTALE, "old gen stale after reuse");
    check(kz_adapter_destroy(h2) == KZ_OK, "cleanup reuse");
    check(kz_adapter_acquire(0) == KZ_EINVAL, "null handle");
    check(kz_adapter_acquire(0xFFFFFFFFu) == KZ_EINVAL, "wild handle");
}

// 7. double destruction
static void t_double_destroy(void) {
    kz_surface_desc_t d = good_desc(16, 16);
    kz_handle_t h = 0;
    check(kz_adapter_create(&d, &h) == KZ_OK, "dbl setup");
    check(kz_adapter_destroy(h) == KZ_OK, "first destroy");
    check(kz_adapter_destroy(h) == KZ_ESTALE, "second destroy stale");
}

// 8. backend allocation failure
static void t_alloc_fail(void) {
    kz_surface_desc_t d = good_desc(16, 16);
    kz_handle_t h = 0;
    kz_handle_t held[KZ_ADAPTER_MAX_RES];
    unsigned nheld = 0;
    unsigned before = live_surfaces;
    int i;
    fail_create = 1;
    check(kz_adapter_create(&d, &h) == KZ_ENOMEM, "backend fail -> ENOMEM");
    fail_create = 0;
    check(live_surfaces == before, "failed create leaks nothing");
    for (i = 0; i < KZ_ADAPTER_MAX_RES; i++) {
        kz_handle_t t = 0;
        if (kz_adapter_create(&d, &t) != KZ_OK) break;
        held[nheld++] = t;
    }
    check(nheld == KZ_ADAPTER_MAX_RES, "table fills exactly");
    check(kz_adapter_create(&d, &h) == KZ_EBUSY, "table full -> EBUSY");
    for (i = 0; i < (int)nheld; i++)
        check(kz_adapter_destroy(held[i]) == KZ_OK, "table drains");
    check(live_surfaces == before, "drained table leaks nothing");
}

// 9. upload failure paths
static void t_upload_fail(void) {
    kz_surface_desc_t d = good_desc(16, 16);
    kz_handle_t h = 0;
    kz_rect_t ok = {0, 0, 16, 16};
    kz_rect_t oob = {8, 8, 16, 16};
    kz_rect_t neg = {-1, 0, 4, 4};
    kz_rect_t empty = {0, 0, 0, 4};
    uint32_t state = 0;
    check(kz_adapter_create(&d, &h) == KZ_OK, "upl setup");
    check(kz_adapter_acquire(h) == KZ_OK, "upl acquire");
    check(kz_adapter_upload(h, NULL, 16, &ok) == KZ_EINVAL, "null pixels");
    check(kz_adapter_upload(h, pixels, 0, &ok) == KZ_EINVAL, "zero pitch");
    check(kz_adapter_upload(h, pixels, 16, &oob) == KZ_EINVAL, "oob rect");
    check(kz_adapter_upload(h, pixels, 16, &neg) == KZ_EINVAL, "neg rect");
    check(kz_adapter_upload(h, pixels, 16, &empty) == KZ_EINVAL, "empty rect");
    check(kz_adapter_upload(h, pixels, 16, NULL) == KZ_EINVAL, "null rect");
    check(kz_adapter_get(h, NULL, &state) == KZ_OK, "get after rejects");
    check(state == KZ_SURF_WRITABLE, "state stays writable after reject");
    check(kz_adapter_upload(h, pixels, 16, &ok) == KZ_OK, "retry works");
    check(kz_adapter_destroy(h) == KZ_OK, "upl cleanup");
}

// 10. present failure
static void t_present_fail(void) {
    kz_handle_t h = make_ready(16, 16);
    kz_rect_t r = {0, 0, 16, 16};
    uint32_t state = 0;
    fail_present = 1;
    check(kz_adapter_present(h, &r) == KZ_EBUSY, "rejected submit -> EBUSY");
    fail_present = 0;
    check(kz_adapter_get(h, NULL, &state) == KZ_OK, "get after reject");
    check(state == KZ_SURF_READY, "stays ready after reject");
    check(kz_adapter_present(h, &r) == KZ_OK, "retry accepted");
    check(last_present_null == 0, "rect passed through");
    check(last_present_rect.w == 16 && last_present_rect.h == 16,
          "present rect exact");
    complete_all();
    check(kz_adapter_poll(h) == KZ_OK, "poll ok");
    check(kz_adapter_destroy(h) == KZ_OK, "present cleanup");
}

// 11. destroy while in flight -> quarantine
static void t_inflight_destroy(void) {
    kz_handle_t h = make_ready(16, 16);
    unsigned live_before;
    check(kz_adapter_present(h, NULL) == KZ_OK, "inflight present");
    live_before = live_surfaces;
    check(kz_adapter_destroy(h) == KZ_OK, "destroy in-flight ok");
    check(kz_adapter_acquire(h) == KZ_ESTALE, "handle dead after destroy");
    check(live_surfaces == live_before, "backend surface retained");
    complete_all();
    {
        kz_surface_desc_t d = good_desc(4, 4);
        kz_handle_t t = 0;
        check(kz_adapter_create(&d, &t) == KZ_OK, "create retires garbage");
        check(kz_adapter_destroy(t) == KZ_OK, "cleanup trigger");
    }
    check(live_surfaces == live_before - 1, "quarantine freed on retire");
}

// 12. fence timeout then late completion
static void t_timeout_late(void) {
    kz_handle_t h = make_ready(16, 16);
    check(kz_adapter_present(h, NULL) == KZ_OK, "timeout present");
    check(kz_adapter_poll(h) == KZ_EBUSY, "pending poll -> EBUSY");
    check(kz_adapter_acquire(h) == KZ_ESTATE, "no rewrite while in flight");
    check(kz_adapter_poll(h) == KZ_EBUSY, "still pending, still busy");
    complete_all();
    check(kz_adapter_poll(h) == KZ_OK, "late completion retires");
    check(kz_adapter_acquire(h) == KZ_OK, "rewrite after release");
    check(kz_adapter_destroy(h) == KZ_OK, "timeout cleanup");
}

// 13. resize and replacement
static void t_resize(void) {
    kz_handle_t h = make_ready(16, 16);
    kz_surface_desc_t big = good_desc(32, 32);
    kz_surface_desc_t bad = good_desc(0, 8);
    kz_handle_t n = 0;
    kz_surface_desc_t got;
    check(kz_adapter_resize(h, &bad, &n) == KZ_EINVAL, "bad resize desc");
    check(kz_adapter_resize(h, &big, &n) == KZ_OK, "resize ok");
    check(n != h, "resize bumps generation");
    check(kz_adapter_acquire(h) == KZ_ESTALE, "old handle stale");
    check(kz_adapter_get(n, &got, NULL) == KZ_OK, "new desc live");
    check(got.width == 32, "new dims applied");
    check(kz_adapter_destroy(n) == KZ_OK, "resize cleanup");
}

// 14. partial-init cleanup is covered in t_alloc_fail (slot freed on
// backend failure); here: table-full drain leaves zero leaks.
static void t_table_drain(void) {
    // Placeholder assertion happens globally: live_surfaces must return
    // to baseline at end of main. This case documents the invariant.
    check(1, "leak invariant checked at exit");
}

// 15. legacy parity: upload/present passthrough is exact.
static void t_passthrough(void) {
    kz_surface_desc_t d = good_desc(16, 16);
    kz_handle_t h = 0;
    kz_rect_t r = {2, 3, 5, 7};
    unsigned i;
    for (i = 0; i < 64 * 64; i++) pixels[i] = 0x11223344u + i;
    upload_count = 0;
    check(kz_adapter_create(&d, &h) == KZ_OK, "pass setup");
    check(kz_adapter_acquire(h) == KZ_OK, "pass acquire");
    check(kz_adapter_upload(h, pixels, 16, &r) == KZ_OK, "pass upload");
    check(upload_count == 1, "exactly one backend upload");
    check(last_upload_src == pixels, "pixel pointer passthrough");
    check(last_upload_rect.x == 2 && last_upload_rect.y == 3 &&
              last_upload_rect.w == 5 && last_upload_rect.h == 7,
          "upload rect exact");
    check(kz_adapter_present(h, NULL) == KZ_OK, "pass present");
    check(last_present_null == 1, "null rect = full present");
    complete_all();
    check(kz_adapter_poll(h) == KZ_OK, "pass poll");
    check(kz_adapter_destroy(h) == KZ_OK, "pass cleanup");
}

int main(void) {
    unsigned surf_base = live_surfaces;
    unsigned heap_base = live_heap;
    t_create_valid();
    t_create_dims();
    t_create_stride();
    t_create_format();
    t_lifecycle();
    t_stale();
    t_double_destroy();
    t_alloc_fail();
    t_upload_fail();
    t_present_fail();
    t_inflight_destroy();
    t_timeout_late();
    t_resize();
    t_table_drain();
    t_passthrough();
    check(live_surfaces == surf_base, "no surface leaks");
    check(live_heap == heap_base, "no heap leaks");
    printf("kz-adapter failures=%u\n", failures);
    return failures ? 1 : 0;
}
