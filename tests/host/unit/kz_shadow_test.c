// R2 differential test: kz_shadow_compose vs an independent per-pixel
// oracle (different code path, same documented semantics) + full vs
// incremental equality. Byte-for-byte required: all ops are exact integer.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kz_shadow.h"

static unsigned failures;
static unsigned mismatch_reports;
static void check(int ok, const char *label) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", label);
    }
}

#define TW 64
#define TH 48
static uint32_t out_a[TW * TH];
static uint32_t out_b[TW * TH];
static kz_surface_desc_t tdesc;

static void *t_alloc(uint32_t n) {
    return malloc(n ? n : 1);
}
static void t_free(void *p) {
    free(p);
}

// Independent oracle: straight per-pixel src-over, no kz_render calls.
static uint32_t oracle_blend(uint32_t dst, uint32_t src, int src_argb,
                              uint32_t opacity, int dst_argb) {
    uint32_t sa = src_argb ? ((src >> 24) & 0xFF) : 255;
    uint32_t a = (sa * opacity + 127u) / 255u;
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t na = 255u - a;
    uint32_t r = (sr * a + dr * na + 127u) / 255u;
    uint32_t g = (sg * a + dg * na + 127u) / 255u;
    uint32_t b = (sb * a + db * na + 127u) / 255u;
    if (a >= 255) {
        if (dst_argb) return (0xFFu << 24) | (sr << 16) | (sg << 8) | sb;
        return (sr << 16) | (sg << 8) | sb;
    }
    if (dst_argb) {
        uint32_t da = (dst >> 24) & 0xFF;
        uint32_t ao = a + (da * na + 127u) / 255u;
        return (ao << 24) | (r << 16) | (g << 8) | b;
    }
    return (r << 16) | (g << 8) | b;
}

static void oracle_render(uint32_t *dst, const kz_layer_t *layers,
                           uint32_t n, uint32_t bg, int dst_argb) {
    uint32_t order[16];
    uint32_t i, j;
    int x, y;
    for (i = 0; i < (uint32_t)(TW * TH); i++)
        dst[i] = dst_argb ? ((0xFFu << 24) | (bg & 0xFFFFFFu)) : (bg & 0xFFFFFFu);
    for (i = 0; i < n; i++) order[i] = i;
    for (i = 1; i < n; i++) {
        uint32_t key = order[i];
        j = i;
        while (j > 0 && layers[order[j - 1]].z > layers[key].z) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    for (i = 0; i < n; i++) {
        const kz_layer_t *l = &layers[order[i]];
        int src_argb;
        if (!l->visible || l->opacity == 0) continue;
        src_argb = (l->desc.format == KZ_FMT_ARGB8888);
        for (y = 0; y < TH; y++) {
            for (x = 0; x < TW; x++) {
                int lx = x - l->x, ly = y - l->y;
                uint32_t s;
                if (lx < 0 || ly < 0) continue;
                if ((uint32_t)lx >= l->desc.width || (uint32_t)ly >= l->desc.height)
                    continue;
                s = l->pixels[(uint32_t)ly * l->desc.stride_px + (uint32_t)lx];
                dst[y * TW + x] = oracle_blend(dst[y * TW + x], s, src_argb,
                                                l->opacity, dst_argb);
            }
        }
    }
}

static void make_desc(kz_surface_desc_t *d, uint32_t w, uint32_t h,
                      uint32_t fmt) {
    d->width = w;
    d->height = h;
    d->stride_px = w;
    d->format = fmt;
    d->size_bytes = (uint64_t)w * h * 4;
    d->usage = KZ_USAGE_CPU_READ | KZ_USAGE_CPU_WRITE;
    d->id = 1;
    d->generation = 0;
    d->owner = 0;
}

static uint32_t layer_red[20 * 12];
static uint32_t layer_blue[40 * 30];
static uint32_t layer_green[30 * 20];
static uint32_t layer_cursor[12 * 16];

static void fill_layer(uint32_t *p, unsigned n, uint32_t v) {
    unsigned i;
    for (i = 0; i < n; i++) p[i] = v;
}

static kz_context_t *begin(uint32_t *fb, uint32_t fmt) {
    kz_context_t *c = NULL;
    make_desc(&tdesc, TW, TH, fmt);
    memset(fb, 0xAA, (size_t)TW * TH * 4);
    if (kz_context_create(&tdesc, fb, t_alloc, t_free, &c) != KZ_OK) return NULL;
    return c;
}

static void compare(const char *label, const uint32_t *a, const uint32_t *b,
                    int xrgb) {
    int x, y;
    for (y = 0; y < TH; y++) {
        for (x = 0; x < TW; x++) {
            uint32_t pa = a[y * TW + x], pb = b[y * TW + x];
            if (xrgb) {
                pa &= 0xFFFFFFu;
                pb &= 0xFFFFFFu;
            }
            if (pa != pb) {
                if (mismatch_reports < 5) {
                    printf("MISMATCH %s at (%d,%d): got 0x%08X want 0x%08X\n",
                           label, x, y, pa, pb);
                    mismatch_reports++;
                }
                failures++;
                return;
            }
        }
    }
}

static void scene_basic(kz_layer_t *L) {
    fill_layer(layer_red, 20 * 12, 0xFF0000u);
    fill_layer(layer_blue, 40 * 30, 0x0000FFu);
    fill_layer(layer_green, 30 * 20, 0x00FF00u);
    make_desc(&L[0].desc, 40, 30, KZ_FMT_XRGB8888);
    L[0].pixels = layer_blue;
    L[0].x = 0;
    L[0].y = 0;
    L[0].z = 0;
    L[0].opacity = 255;
    L[0].visible = 1;
    make_desc(&L[1].desc, 20, 12, KZ_FMT_XRGB8888);
    L[1].pixels = layer_red;
    L[1].x = 5;
    L[1].y = 5;
    L[1].z = 1;
    L[1].opacity = 255;
    L[1].visible = 1;
    make_desc(&L[2].desc, 30, 20, KZ_FMT_XRGB8888);
    L[2].pixels = layer_green;
    L[2].x = 20;
    L[2].y = 10;
    L[2].z = 2;
    L[2].opacity = 128;
    L[2].visible = 1;
}

static void t_full_vs_oracle(void) {
    kz_layer_t L[3];
    kz_context_t *c;
    kz_shadow_stats_t st;
    scene_basic(L);
    c = begin(out_a, KZ_FMT_XRGB8888);
    check(c != NULL, "ctx for full");
    check(kz_shadow_compose(c, &tdesc, L, 3, NULL, 0, 0x101010u, &st) == KZ_OK,
          "full compose");
    check(st.failed_index == KZ_SHADOW_NO_FAIL, "no fail idx");
    check(st.layers_drawn == 3, "3 layers drawn");
    oracle_render(out_b, L, 3, 0x101010u, 0);
    compare("full-vs-oracle", out_a, out_b, 1);
    check(out_a[5 * TW + 5] == 0xFF0000u, "opaque red on top");
    kz_context_destroy(c, t_free);
}

static void t_incremental(void) {
    kz_layer_t L[3];
    kz_context_t *ca, *cb;
    kz_rect_t dmg[5] = {{0, 0, 10, 10},
                        {20, 10, 8, 8},
                        {50, 0, 14, 48},
                        {0, 40, 64, 8},
                        {30, 20, 4, 4}};
    scene_basic(L);
    ca = begin(out_a, KZ_FMT_XRGB8888);
    cb = begin(out_b, KZ_FMT_XRGB8888);
    check(ca && cb, "ctx pair");
    check(kz_shadow_compose(ca, &tdesc, L, 3, NULL, 0, 0x101010u, NULL) == KZ_OK,
          "incremental base full");
    // damage only the listed rects on cb by pre-filling base then composing
    check(kz_shadow_compose(cb, &tdesc, L, 3, dmg, 5, 0x101010u, NULL) == KZ_OK,
          "incremental damage");
    // outside damage, cb still holds 0xAA sentinel: verify damage rects match
    {
        int k;
        int ok = 1;
        for (k = 0; k < 5 && ok; k++) {
            int x, y;
            for (y = dmg[k].y; y < dmg[k].y + dmg[k].h && ok; y++)
                for (x = dmg[k].x; x < dmg[k].x + dmg[k].w && ok; x++)
                    if ((out_a[y * TW + x] & 0xFFFFFFu) !=
                        (out_b[y * TW + x] & 0xFFFFFFu))
                        ok = 0;
        }
        check(ok, "damage rects match full render");
    }
    kz_context_destroy(ca, t_free);
    kz_context_destroy(cb, t_free);
}

static void t_repaint_stable(void) {
    kz_layer_t L[3];
    kz_context_t *c;
    uint32_t snap[TW * TH];
    scene_basic(L);
    c = begin(out_a, KZ_FMT_XRGB8888);
    check(kz_shadow_compose(c, &tdesc, L, 3, NULL, 0, 0x101010u, NULL) == KZ_OK,
          "repaint first");
    memcpy(snap, out_a, sizeof(snap));
    check(kz_shadow_compose(c, &tdesc, L, 3, NULL, 0, 0x101010u, NULL) == KZ_OK,
          "repaint second");
    compare("repaint-stable", out_a, snap, 1);
    kz_context_destroy(c, t_free);
}

static void t_offscreen(void) {
    kz_layer_t L[2];
    kz_context_t *c;
    scene_basic(L);
    L[0].x = -5;
    L[0].y = -5;
    L[1].x = 60;
    L[1].y = 40;
    L[2 - 1].opacity = 255;
    c = begin(out_a, KZ_FMT_XRGB8888);
    check(kz_shadow_compose(c, &tdesc, L, 2, NULL, 0, 0x000000u, NULL) == KZ_OK,
          "offscreen compose");
    oracle_render(out_b, L, 2, 0x000000u, 0);
    compare("offscreen-vs-oracle", out_a, out_b, 1);
    kz_context_destroy(c, t_free);
}

static void t_destroy_replace(void) {
    kz_layer_t L[3];
    kz_context_t *c;
    kz_shadow_stats_t st;
    scene_basic(L);
    L[1].visible = 0;
    L[1].pixels = NULL;
    c = begin(out_a, KZ_FMT_XRGB8888);
    check(kz_shadow_compose(c, &tdesc, L, 3, NULL, 0, 0x101010u, &st) == KZ_OK,
          "destroyed layer skipped");
    check(st.layers_skipped == 1, "one skipped");
    oracle_render(out_b, L, 3, 0x101010u, 0);
    compare("destroy-vs-oracle", out_a, out_b, 1);
    L[1].visible = 1;
    L[1].pixels = layer_red;
    L[1].x = 40;
    check(kz_shadow_compose(c, &tdesc, L, 3, NULL, 0, 0x101010u, NULL) == KZ_OK,
          "replaced layer");
    oracle_render(out_b, L, 3, 0x101010u, 0);
    compare("replace-vs-oracle", out_a, out_b, 1);
    kz_context_destroy(c, t_free);
}

static void t_cursor_damage(void) {
    kz_layer_t L[2];
    kz_context_t *c;
    kz_rect_t dmg[2] = {{10, 10, 12, 16}, {14, 12, 12, 16}};
    unsigned i;
    for (i = 0; i < 12u * 16u; i++)
        layer_cursor[i] = (i % 3 == 0) ? 0xC0FFFFFFu : 0x00000000u;
    scene_basic(L);
    for (i = 0; i < 40u * 30u; i++) layer_blue[i] |= 0xFF000000u;
    for (i = 0; i < 20u * 12u; i++) layer_red[i] |= 0xFF000000u;
    L[0].desc.format = KZ_FMT_ARGB8888;
    make_desc(&L[1].desc, 12, 16, KZ_FMT_ARGB8888);
    L[1].pixels = layer_cursor;
    L[1].x = 14;
    L[1].y = 12;
    L[1].z = 5;
    L[1].opacity = 255;
    L[1].visible = 1;
    c = begin(out_a, KZ_FMT_ARGB8888);
    check(kz_shadow_compose(c, &tdesc, L, 2, dmg, 2, 0x101010u, NULL) == KZ_OK,
          "cursor damage compose");
    {
        kz_context_t *cf = begin(out_b, KZ_FMT_ARGB8888);
        kz_rect_t bound = {10, 10, 16, 18};
        check(kz_shadow_compose(cf, &tdesc, L, 2, &bound, 1, 0x101010u, NULL) ==
                  KZ_OK,
              "cursor bound compose");
        {
            int x, y, ok = 1;
            // independent oracle over the same ARGB scene for the bound
            static uint32_t ref[TW * TH];
            oracle_render(ref, L, 2, 0x101010u, 1);
            for (y = 10; y < 28 && ok; y++)
                for (x = 10; x < 26 && ok; x++) {
                    int in0 = (x >= 10 && x < 22 && y >= 10 && y < 26);
                    int in1 = (x >= 14 && x < 26 && y >= 12 && y < 28);
                    if ((in0 || in1) &&
                        (out_a[y * TW + x] != out_b[y * TW + x]))
                        ok = 0;
                    if ((in0 || in1) &&
                        (out_a[y * TW + x] != ref[y * TW + x]))
                        ok = 0;
                }
            check(ok, "cursor damage union matches bound+oracle");
        }
        kz_context_destroy(cf, t_free);
    }
    kz_context_destroy(c, t_free);
}

static void t_overflow_collapse(void) {
    kz_layer_t L[3];
    kz_context_t *ca, *cb;
    kz_rect_t many[70];
    int i;
    scene_basic(L);
    for (i = 0; i < 70; i++) {
        many[i].x = (i * 7) % TW;
        many[i].y = (i * 5) % TH;
        many[i].w = 8;
        many[i].h = 8;
    }
    ca = begin(out_a, KZ_FMT_XRGB8888);
    cb = begin(out_b, KZ_FMT_XRGB8888);
    check(kz_shadow_compose(ca, &tdesc, L, 3, many, 70, 0x101010u, NULL) ==
              KZ_OK,
          "70 rects collapse");
    check(kz_shadow_compose(cb, &tdesc, L, 3, NULL, 0, 0x101010u, NULL) ==
              KZ_OK,
          "full reference");
    // Collapse is a superset of the union: every pixel inside the union
    // must match full; every pixel outside the bounding box must be
    // untouched sentinel (proves no over-write beyond the superset).
    {
        int x0 = TW, y0 = TH, x1 = 0, y1 = 0;
        int x, y, ok = 1;
        for (i = 0; i < 70; i++) {
            int rx0 = many[i].x < 0 ? 0 : many[i].x;
            int ry0 = many[i].y < 0 ? 0 : many[i].y;
            int rx1 = many[i].x + many[i].w > TW ? TW : many[i].x + many[i].w;
            int ry1 = many[i].y + many[i].h > TH ? TH : many[i].y + many[i].h;
            if (rx0 < x0) x0 = rx0;
            if (ry0 < y0) y0 = ry0;
            if (rx1 > x1) x1 = rx1;
            if (ry1 > y1) y1 = ry1;
        }
        for (y = 0; y < TH && ok; y++) {
            for (x = 0; x < TW && ok; x++) {
                uint32_t pa = out_a[y * TW + x], pb = out_b[y * TW + x];
                int in = (x >= x0 && x < x1 && y >= y0 && y < y1);
                if (in) {
                    if ((pa & 0xFFFFFFu) != (pb & 0xFFFFFFu)) ok = 0;
                } else if (pa != 0xAAAAAAAAu) {
                    ok = 0;
                }
            }
        }
        check(ok, "overflow collapse: union matches, outside untouched");
    }
    kz_context_destroy(ca, t_free);
    kz_context_destroy(cb, t_free);
}

static void t_errors(void) {
    kz_layer_t L[3];
    kz_context_t *c;
    kz_shadow_stats_t st;
    kz_rect_t neg = {-1, 0, 4, 4};
    uint32_t sentinel[TW * TH];
    unsigned i;
    scene_basic(L);
    for (i = 0; i < (unsigned)(TW * TH); i++) sentinel[i] = 0xDEADBEEFu;
    c = begin(out_a, KZ_FMT_XRGB8888);
    memcpy(out_a, sentinel, sizeof(sentinel));
    L[2].pixels = NULL;
    check(kz_shadow_compose(c, &tdesc, L, 3, NULL, 0, 0, &st) == KZ_EINVAL,
          "null pixels rejected");
    check(st.failed_index == 2, "failing index reported");
    check(!memcmp(out_a, sentinel, sizeof(sentinel)), "target untouched");
    L[2].pixels = layer_green;
    L[2].desc.format = 99;
    check(kz_shadow_compose(c, &tdesc, L, 3, NULL, 0, 0, &st) == KZ_EINVAL,
          "bad format rejected");
    {
        uint32_t argb_px[2 * 2];
        kz_layer_t M[1];
        fill_layer(argb_px, 4, 0x800000FFu);
        make_desc(&M[0].desc, 2, 2, KZ_FMT_ARGB8888);
        M[0].pixels = argb_px;
        M[0].x = 0;
        M[0].y = 0;
        M[0].z = 0;
        M[0].opacity = 255;
        M[0].visible = 1;
        check(kz_shadow_compose(c, &tdesc, M, 1, NULL, 0, 0, &st) == KZ_ENOSUP,
              "cross-format layer refused, never faked");
        check(st.failed_index == 0, "cross-format index reported");
    }
    L[2].desc.format = KZ_FMT_XRGB8888;
    check(kz_shadow_compose(c, &tdesc, L, 17, NULL, 0, 0, NULL) == KZ_EINVAL,
          "too many layers");
    check(kz_shadow_compose(c, &tdesc, L, 3, &neg, 1, 0, NULL) == KZ_OK,
          "negative-origin damage clipped, not an error");
    check((out_a[0] & 0xFFFFFFu) != 0xADBEEFu, "clipped region rendered");
    memcpy(out_a, sentinel, sizeof(sentinel));
    {
        kz_rect_t empty = {4, 4, 0, 4};
        check(kz_shadow_compose(c, &tdesc, L, 3, &empty, 1, 0, NULL) == KZ_OK,
              "empty damage ok");
    }
    check(!memcmp(out_a, sentinel, sizeof(sentinel)), "still untouched");
    kz_context_destroy(c, t_free);
}

static void t_spot_exact(void) {
    // Hand-computed: 128/255 white over black = (255*128+127)/255 = 128.
    kz_layer_t L[1];
    kz_context_t *c;
    uint32_t white[4 * 4];
    fill_layer(white, 16, 0xFFFFFFu);
    make_desc(&L[0].desc, 4, 4, KZ_FMT_XRGB8888);
    L[0].pixels = white;
    L[0].x = 0;
    L[0].y = 0;
    L[0].z = 0;
    L[0].opacity = 128;
    L[0].visible = 1;
    c = begin(out_a, KZ_FMT_XRGB8888);
    check(kz_shadow_compose(c, &tdesc, L, 1, NULL, 0, 0x000000u, NULL) == KZ_OK,
          "spot compose");
    check(out_a[0] == 0x808080u, "128-over-black exact 0x808080");
    kz_context_destroy(c, t_free);
}

int main(void) {
    t_full_vs_oracle();
    t_incremental();
    t_repaint_stable();
    t_offscreen();
    t_destroy_replace();
    t_cursor_damage();
    t_overflow_collapse();
    t_errors();
    t_spot_exact();
    printf("kz-shadow failures=%u\n", failures);
    return failures ? 1 : 0;
}
