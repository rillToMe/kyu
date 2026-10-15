// shadow_cons.c - consumer-side shadow compose for shm_demo.
// Same libs/render sources as the host-tested implementation.
#include "userlib.h"
#include "kz_shadow.h"

#define W 64
#define H 48

uint32_t kz_ck(const uint32_t *p, unsigned n) {
    uint32_t h = 2166136261u;
    unsigned i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void *ra(uint32_t n) {
    return sys_alloc(n);
}
static void rf(void *p) {
    sys_free(p);
}

int kz_shadow_consumer(const uint32_t *src, uint32_t *dst, uint32_t *ck) {
    kz_surface_desc_t sdesc, tdesc;
    kz_context_t *ctx = 0;
    kz_layer_t layer;
    kz_shadow_stats_t st;
    int ok;

    sdesc.width = W;
    sdesc.height = H;
    sdesc.stride_px = W;
    sdesc.format = KZ_FMT_XRGB8888;
    sdesc.size_bytes = (uint64_t)W * H * 4u;
    sdesc.usage = KZ_USAGE_CPU_READ;
    sdesc.id = 0;
    sdesc.generation = 0;
    sdesc.owner = 0;
    tdesc = sdesc;
    tdesc.usage = KZ_USAGE_CPU_READ | KZ_USAGE_CPU_WRITE;

    if (kz_context_create(&tdesc, dst, ra, rf, &ctx) != KZ_OK) return 0;
    layer.pixels = src;
    layer.desc = sdesc;
    layer.x = 0;
    layer.y = 0;
    layer.z = 0;
    layer.opacity = 255;
    layer.visible = 1;
    ok = (kz_shadow_compose(ctx, &tdesc, &layer, 1, 0, 0, 0u, &st) == KZ_OK);
    kz_context_destroy(ctx, rf);
    if (!ok) return 0;
    if (st.failed_index != KZ_SHADOW_NO_FAIL) return 0;
    if (st.layers_drawn != 1) return 0;
    *ck = kz_ck(dst, W * H);
    return 1;
}
