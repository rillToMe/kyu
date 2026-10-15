// R1 native 2D API tests (software reference, RAM framebuffer only).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kz_render.h"

static unsigned failures;
static void check(int ok, const char *label) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", label);
    }
}

#define W 32
#define H 16
static uint32_t fb[W * H];
static kz_surface_desc_t desc;
static kz_context_t *ctx;

static void *t_alloc(uint32_t n) {
    return malloc(n ? n : 1);
}

static void t_free(void *p) {
    free(p);
}

static uint32_t px(int x, int y) {
    return fb[(unsigned)y * W + (unsigned)x];
}

static void setup(void) {
    memset(fb, 0, sizeof(fb));
    check(kz_target_wrap(fb, W, H, W, KZ_FMT_XRGB8888, &desc) == KZ_OK,
          "wrap 32x16");
    check(kz_context_create(&desc, fb, t_alloc, t_free, &ctx) == KZ_OK,
          "context create");
}

static void teardown(void) {
    kz_context_destroy(ctx, t_free);
    ctx = NULL;
}

static kz_paint_t paint(uint32_t c, uint32_t a) {
    kz_paint_t p;
    p.color = c;
    p.alpha = a;
    return p;
}

static void null_checks(void) {
    kz_rect_t r = {0, 0, 2, 2};
    kz_paint_t p = paint(0xFF0000, 255);
    check(kz_context_create(NULL, fb, t_alloc, t_free, &ctx) == KZ_EINVAL,
          "null target");
    check(kz_context_create(&desc, NULL, t_alloc, t_free, &ctx) == KZ_EINVAL,
          "null pixels");
    check(kz_context_create(&desc, fb, t_alloc, t_free, NULL) == KZ_EINVAL,
          "null out");
    check(kz_context_create(&desc, fb, NULL, t_free, &ctx) == KZ_EINVAL,
          "null alloc");
    check(kz_context_create(&desc, fb, t_alloc, NULL, &ctx) == KZ_EINVAL,
          "null free");
    check(kz_fill_rect(NULL, &r, &p) == KZ_EINVAL, "null ctx fill");
    check(kz_fill_rect(ctx, NULL, &p) == KZ_EINVAL, "null rect fill");
    check(kz_fill_rect(ctx, &r, NULL) == KZ_EINVAL, "null paint fill");
    check(kz_flush(NULL, NULL) == KZ_EINVAL, "null ctx flush");
    check(kz_save(NULL) == KZ_EINVAL, "null ctx save");
    check(kz_restore(NULL) == KZ_EINVAL, "null ctx restore");
    check(kz_clear(NULL, 0) == KZ_EINVAL, "null ctx clear");
}

static void fill_checks(void) {
    kz_rect_t r = {2, 3, 4, 5};
    kz_paint_t red = paint(0xFF0000, 255);
    check(kz_fill_rect(ctx, &r, &red) == KZ_OK, "fill ok");
    check(px(2, 3) == 0xFF0000, "fill top-left");
    check(px(5, 7) == 0xFF0000, "fill bottom-right");
    check(px(1, 3) == 0, "fill leaves left neighbour");
    check(px(6, 3) == 0, "fill leaves right neighbour");

    r.x = 0;
    r.y = 0;
    r.w = 0;
    r.h = 5;
    check(kz_fill_rect(ctx, &r, &red) == KZ_OK, "empty rect no-op ok");
    r.w = -1;
    check(kz_fill_rect(ctx, &r, &red) == KZ_EINVAL, "negative w rejected");

    r.x = W - 2;
    r.y = H - 2;
    r.w = 10;
    r.h = 10;
    check(kz_fill_rect(ctx, &r, &red) == KZ_OK, "oob rect clipped ok");
    check(px(W - 1, H - 1) == 0xFF0000, "oob clipped pixel set");

    r.x = -50;
    r.y = -50;
    r.w = 4;
    r.h = 4;
    check(kz_fill_rect(ctx, &r, &red) == KZ_OK, "fully-oob rect ok");
}

static void alpha_checks(void) {
    kz_rect_t r = {0, 0, W, H};
    kz_paint_t half = paint(0xFF0000, 128);
    kz_paint_t full = paint(0x00FF00, 255);
    check(kz_fill_rect(ctx, &r, &full) == KZ_OK, "green base");
    check(kz_fill_rect(ctx, &r, &half) == KZ_OK, "half red over green");
    check(px(0, 0) == 0x807F00, "128-over blend exact");
    check(kz_clear(ctx, 0x0000FF) == KZ_OK, "clear blue");
    check(px(W - 1, H - 1) == 0x0000FF, "clear covers all");
}

static void repaint_checks(void) {
    kz_rect_t r = {5, 5, 3, 3};
    kz_paint_t blue = paint(0x0000FF, 255);
    int i;
    check(kz_clear(ctx, 0) == KZ_OK, "clear black");
    for (i = 0; i < 3; i++)
        check(kz_fill_rect(ctx, &r, &blue) == KZ_OK, "repaint ok");
    check(px(5, 5) == 0x0000FF, "opaque repaint idempotent");
}

static void border_checks(void) {
    kz_rect_t r = {1, 1, 4, 4};
    kz_paint_t white = paint(0xFFFFFF, 255);
    check(kz_clear(ctx, 0) == KZ_OK, "clear for border");
    check(kz_draw_rect(ctx, &r, &white) == KZ_OK, "border ok");
    check(px(1, 1) == 0xFFFFFF, "border corner");
    check(px(4, 4) == 0xFFFFFF, "border far corner");
    check(px(2, 2) == 0, "border hollow center");
    check(px(0, 0) == 0, "border outside untouched");
}

static void clip_checks(void) {
    kz_rect_t full = {0, 0, W, H};
    kz_rect_t tiny = {4, 4, 2, 2};
    kz_paint_t red = paint(0xFF0000, 255);
    check(kz_clear(ctx, 0) == KZ_OK, "clear for clip");
    check(kz_save(ctx) == KZ_OK, "save for clip");
    check(kz_clip_rect(ctx, &tiny) == KZ_OK, "clip tiny");
    check(kz_fill_rect(ctx, &full, &red) == KZ_OK, "fill under clip");
    check(px(4, 4) == 0xFF0000, "clip inside set");
    check(px(3, 4) == 0, "clip left blocked");
    check(px(6, 4) == 0, "clip right blocked");
    check(kz_restore(ctx) == KZ_OK, "restore clip");
    check(kz_fill_rect(ctx, &full, &red) == KZ_OK, "fill after restore");
    check(px(0, 0) == 0xFF0000, "restore reopens clip");
    check(kz_restore(ctx) == KZ_ESTATE, "restore underflow rejected");
}

static void transform_checks(void) {
    kz_rect_t r = {1, 1, 2, 2};
    kz_rect_t full = {0, 0, W, H};
    kz_paint_t red = paint(0xFF0000, 255);
    kz_transform_t t;
    check(kz_clear(ctx, 0) == KZ_OK, "clear for transform");
    t.sx = 2 * KZ_FIXED_ONE;
    t.sy = 2 * KZ_FIXED_ONE;
    t.tx = 10;
    t.ty = 0;
    check(kz_set_transform(ctx, &t) == KZ_OK, "scale2+translate");
    check(kz_fill_rect(ctx, &r, &red) == KZ_OK, "fill transformed");
    check(px(12, 2) == 0xFF0000, "transformed origin");
    check(px(15, 5) == 0xFF0000, "transformed extent");
    check(px(1, 1) == 0, "untransformed area untouched");
    check(kz_save(ctx) == KZ_OK, "save transform");
    t.sx = KZ_FIXED_ONE;
    t.sy = KZ_FIXED_ONE;
    t.tx = 0;
    t.ty = 0;
    check(kz_set_transform(ctx, &t) == KZ_OK, "identity inside save");
    check(kz_fill_rect(ctx, &full, &red) == KZ_OK, "fill identity");
    check(kz_restore(ctx) == KZ_OK, "restore transform");
    t.sx = 0;
    t.sy = KZ_FIXED_ONE;
    t.tx = 0;
    t.ty = 0;
    check(kz_set_transform(ctx, &t) == KZ_EINVAL, "zero scale rejected");
    t.sx = -KZ_FIXED_ONE;
    check(kz_set_transform(ctx, &t) == KZ_EINVAL, "negative scale rejected");
    check(kz_fill_rect(ctx, &r, &red) == KZ_OK, "old transform intact");
    check(px(12, 2) == 0xFF0000, "transform survived bad set");
}

static void blit_checks(void) {
    uint32_t src[4 * 4];
    kz_surface_desc_t sdesc;
    kz_rect_t sr = {0, 0, 4, 4};
    kz_rect_t dr = {8, 8, 4, 4};
    kz_rect_t bad = {8, 8, 5, 5};
    kz_paint_t op = paint(0, 255);
    kz_transform_t ident = {KZ_FIXED_ONE, KZ_FIXED_ONE, 0, 0};
    unsigned i;
    check(kz_set_transform(ctx, &ident) == KZ_OK, "identity for blit");
    for (i = 0; i < 16; i++) src[i] = 0x123456;
    check(kz_target_wrap(src, 4, 4, 4, KZ_FMT_XRGB8888, &sdesc) == KZ_OK,
          "wrap blit src");
    check(kz_clear(ctx, 0) == KZ_OK, "clear for blit");
    check(kz_blit(ctx, src, &sdesc, &sr, &dr, &op) == KZ_OK, "blit ok");
    check(px(8, 8) == 0x123456, "blit dst pixel");
    check(px(11, 11) == 0x123456, "blit dst far");
    check(px(7, 8) == 0, "blit neighbour clean");
    check(kz_blit(ctx, src, &sdesc, &sr, &bad, &op) == KZ_ENOSUP,
          "scaling blit refused");
    sr.w = 5;
    check(kz_blit(ctx, src, &sdesc, &sr, &dr, &op) == KZ_EINVAL,
          "src-rect overflow refused");
    sr.w = 4;
    check(kz_blit(ctx, src, &sdesc, &sr, &dr, NULL) == KZ_EINVAL,
          "null paint blit refused");
}

static void overlap_checks(void) {
    kz_surface_desc_t self;
    kz_rect_t sr = {0, 0, 8, 4};
    kz_rect_t dr = {2, 1, 8, 4};
    kz_rect_t full = {0, 0, 8, 4};
    kz_paint_t op = paint(0, 255);
    unsigned x;
    check(kz_clear(ctx, 0) == KZ_OK, "clear for overlap");
    for (x = 0; x < 8; x++) {
        kz_rect_t dot = {(int32_t)x, 0, 1, 1};
        kz_paint_t p = paint(0x010101 * (x + 1), 255);
        check(kz_fill_rect(ctx, &dot, &p) == KZ_OK, "stripe");
    }
    check(kz_target_wrap(fb, W, H, W, KZ_FMT_XRGB8888, &self) == KZ_OK,
          "wrap self");
    check(kz_blit(ctx, fb, &self, &sr, &dr, &op) == KZ_OK, "overlap blit");
    check(px(2, 1) == 0x010101, "overlap keeps src order");
    check(px(9, 1) == 0x080808, "overlap tail intact");
    (void)full;
}

static void argb_checks(void) {
    uint32_t asrc[2 * 2];
    kz_surface_desc_t adesc;
    uint32_t abuf[8 * 8];
    kz_surface_desc_t atarget;
    kz_context_t *actx = NULL;
    kz_rect_t sr = {0, 0, 2, 2};
    kz_rect_t dr = {3, 3, 2, 2};
    kz_paint_t op = paint(0, 255);
    asrc[0] = 0xFF0000FFu;
    asrc[1] = 0x8000FF00u;
    asrc[2] = 0x00000000u;
    asrc[3] = 0xFFFFFFFFu;
    check(kz_target_wrap(asrc, 2, 2, 2, KZ_FMT_ARGB8888, &adesc) == KZ_OK,
          "wrap argb src");
    check(kz_target_wrap(abuf, 8, 8, 8, KZ_FMT_ARGB8888, &atarget) == KZ_OK,
          "wrap argb target");
    check(kz_context_create(&atarget, abuf, t_alloc, t_free, &actx) == KZ_OK,
          "argb context");
    check(kz_clear(actx, 0x000000) == KZ_OK, "clear argb black");
    check(abuf[0] == 0xFF000000u, "clear packs opaque alpha");
    check(kz_blit(actx, asrc, &adesc, &sr, &dr, &op) == KZ_OK, "argb blit");
    check(abuf[3 * 8 + 3] == 0xFF0000FFu, "opaque src pixel exact");
    check(abuf[3 * 8 + 4] == 0xFF008000u, "half src blends: a=(128*255+127)/255=128, g=(255*128+127)/255=128");
    check(abuf[4 * 8 + 3] == 0xFF000000u, "transparent src keeps dst");
    check(abuf[4 * 8 + 4] == 0xFFFFFFFFu, "white opaque exact");
    kz_context_destroy(actx, t_free);
}

static void flush_checks(void) {
    kz_fence_t *f = (kz_fence_t *)0x1;
    check(kz_flush(ctx, &f) == KZ_OK, "flush ok");
    check(f == NULL, "software flush has no fence");
    check(kz_flush(ctx, NULL) == KZ_OK, "flush null fence ok");
}

static void wrap_checks(void) {
    kz_surface_desc_t d;
    check(kz_target_wrap(NULL, 4, 4, 4, KZ_FMT_XRGB8888, &d) == KZ_EINVAL,
          "null pixels refused");
    check(kz_target_wrap(fb, 0, 4, 4, KZ_FMT_XRGB8888, &d) == KZ_EINVAL,
          "zero width refused");
    check(kz_target_wrap(fb, 4, 4, 3, KZ_FMT_XRGB8888, &d) == KZ_EINVAL,
          "short stride refused");
    check(kz_target_wrap(fb, 4, 4, 4, 99, &d) == KZ_EINVAL,
          "bad format refused");
    {
        kz_surface_desc_t od;
        uint32_t *opixels = NULL;
        kz_rect_t r = {0, 0, 8, 8};
        kz_paint_t red = paint(0xFF0000, 255);
        kz_context_t *octx = NULL;
        check(kz_target_create(8, 8, KZ_FMT_XRGB8888, NULL, t_free, &od,
                               &opixels) == KZ_EINVAL,
              "create null alloc refused");
        check(kz_target_create(0, 8, KZ_FMT_XRGB8888, t_alloc, t_free, &od,
                               &opixels) == KZ_EINVAL,
              "create zero width refused");
        check(kz_target_create(8, 8, KZ_FMT_XRGB8888, t_alloc, t_free, &od,
                               &opixels) == KZ_OK,
              "create owned ok");
        check(opixels != NULL, "create returns pixels");
        check(kz_context_create(&od, opixels, t_alloc, t_free, &octx) == KZ_OK,
              "context on owned target");
        check(kz_fill_rect(octx, &r, &red) == KZ_OK, "fill owned");
        check(opixels[0] == 0xFF0000, "owned pixel set");
        kz_context_destroy(octx, t_free);
        kz_target_free(&od, opixels, t_free);
    }
}

int main(void) {
    setup();
    null_checks();
    fill_checks();
    alpha_checks();
    repaint_checks();
    border_checks();
    clip_checks();
    transform_checks();
    blit_checks();
    overlap_checks();
    argb_checks();
    flush_checks();
    wrap_checks();
    teardown();
    printf("kz-render failures=%u\n", failures);
    return failures ? 1 : 0;
}
