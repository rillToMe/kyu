// R1 surface + sync contract tests (pure header logic, no hardware).
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "kz_surface.h"
#include "kz_sync.h"

static unsigned failures;
static void check(int ok, const char *label) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", label);
    }
}

static kz_surface_desc_t good(void) {
    kz_surface_desc_t d;
    d.width = 64;
    d.height = 32;
    d.stride_px = 64;
    d.format = KZ_FMT_XRGB8888;
    d.size_bytes = (uint64_t)64 * 32 * 4;
    d.usage = KZ_USAGE_CPU_READ | KZ_USAGE_CPU_WRITE;
    d.id = 7;
    d.generation = 1;
    d.owner = 3;
    return d;
}

static void validation_checks(void) {
    kz_surface_desc_t d = good();
    check(kz_surface_validate(&d) == KZ_OK, "good desc validates");
    check(kz_surface_validate(NULL) == KZ_EINVAL, "null desc rejected");

    d = good();
    d.width = 0;
    check(kz_surface_validate(&d) == KZ_EINVAL, "zero width rejected");
    d = good();
    d.height = 0;
    check(kz_surface_validate(&d) == KZ_EINVAL, "zero height rejected");
    d = good();
    d.width = KZ_SURFACE_MAX_DIM + 1;
    check(kz_surface_validate(&d) == KZ_EINVAL, "oversize width rejected");
    d = good();
    d.format = 99;
    check(kz_surface_validate(&d) == KZ_EINVAL, "unknown format rejected");
    d = good();
    d.usage = 0;
    check(kz_surface_validate(&d) == KZ_EINVAL, "zero usage rejected");
    d = good();
    d.stride_px = d.width - 1;
    check(kz_surface_validate(&d) == KZ_EINVAL, "stride < width rejected");
    d = good();
    d.size_bytes = (uint64_t)64 * 32 * 4 - 1;
    check(kz_surface_validate(&d) == KZ_EINVAL, "short backing rejected");
    d.width = 0xFFFFFFFFu;
    d.height = 0xFFFFFFFFu;
    d.stride_px = 0xFFFFFFFFu;
    d.size_bytes = UINT64_MAX;
    check(kz_surface_validate(&d) != KZ_OK, "u32-max dims rejected");
}

static void bytes_checks(void) {
    check(kz_surface_bytes(64, 32) == (uint64_t)64 * 32 * 4, "bytes exact");
    check(kz_surface_bytes(0, 32) == 0, "zero width needs nothing");
    check(kz_surface_bytes(0xFFFFFFFFu, 0xFFFFFFFFu) == 0, "u32 overflow safe");
}

static void lifecycle_checks(void) {
    check(kz_surface_transition(KZ_SURF_UNINIT, KZ_SURF_CREATED) == KZ_OK,
          "uninit->created");
    check(kz_surface_transition(KZ_SURF_UNINIT, KZ_SURF_READY) == KZ_ESTATE,
          "uninit->ready rejected");
    check(kz_surface_transition(KZ_SURF_CREATED, KZ_SURF_WRITABLE) == KZ_OK,
          "created->writable");
    check(kz_surface_transition(KZ_SURF_CREATED, KZ_SURF_READY) == KZ_ESTATE,
          "created->ready rejected");
    check(kz_surface_transition(KZ_SURF_WRITABLE, KZ_SURF_READY) == KZ_OK,
          "writable->ready");
    check(kz_surface_transition(KZ_SURF_READY, KZ_SURF_IN_USE) == KZ_OK,
          "ready->in_use");
    check(kz_surface_transition(KZ_SURF_IN_USE, KZ_SURF_RELEASED) == KZ_OK,
          "in_use->released");
    check(kz_surface_transition(KZ_SURF_IN_USE, KZ_SURF_WRITABLE) == KZ_ESTATE,
          "in_use->writable rejected (consumer may read)");
    check(kz_surface_transition(KZ_SURF_RELEASED, KZ_SURF_WRITABLE) == KZ_OK,
          "released->writable (next frame)");
    check(kz_surface_transition(KZ_SURF_RELEASED, KZ_SURF_READY) == KZ_ESTATE,
          "released->ready rejected (must rewrite)");
    check(kz_surface_transition(KZ_SURF_DESTROYED, KZ_SURF_CREATED) == KZ_ESTATE,
          "destroyed terminal");
    check(kz_surface_transition(KZ_SURF_READY, KZ_SURF_DESTROYED) == KZ_OK,
          "any live state->destroyed");
    check(kz_surface_check_gen(7, 1, 7, 1) == KZ_OK, "matching gen live");
    check(kz_surface_check_gen(7, 2, 7, 1) == KZ_ESTALE, "old gen stale");
    check(kz_surface_check_gen(7, 1, 8, 1) == KZ_ESTALE, "wrong id stale");
}

static void sync_checks(void) {
    check(kz_stage_reached(KZ_STAGE_RELEASED, KZ_STAGE_SUBMITTED),
          "released proves submitted");
    check(!kz_stage_reached(KZ_STAGE_SUBMITTED, KZ_STAGE_RELEASED),
          "submitted does not prove released");
    check(!kz_stage_reached(KZ_STAGE_RENDER_DONE, KZ_STAGE_CONSUMER_DONE),
          "render done is not consumer done");
    check(kz_stage_sync_submit() == KZ_STAGE_RELEASED,
          "sync submit completes all stages");
    check(kz_fence_released(0) == KZ_OK, "not-pending fence releases");
    check(kz_fence_released(1) == KZ_EBUSY, "pending fence stays busy");
    check(kz_wait_validate(KZ_FENCE_POLL_US) == KZ_OK, "poll budget valid");
    check(kz_wait_validate(0) == KZ_EINVAL, "zero wait rejected");
    check(kz_wait_validate(KZ_FENCE_WAIT_MAX_US + 1) == KZ_EINVAL,
          "over-ceiling wait rejected");
}

int main(void) {
    validation_checks();
    bytes_checks();
    lifecycle_checks();
    sync_checks();
    printf("kz-surface failures=%u\n", failures);
    return failures ? 1 : 0;
}
