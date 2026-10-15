#include "kz_render.h"

#include <stddef.h>
#include <string.h>

struct kz_context {
    uint32_t *pixels;
    uint32_t width, height, stride_px, format;
    kz_transform_t tr;
    kz_rect_t clip;
    kz_transform_t tr_stack[KZ_STATE_MAX];
    kz_rect_t clip_stack[KZ_STATE_MAX];
    uint32_t depth;
};

static int64_t mul1024(int64_t v, int32_t s) {
    return (v * (int64_t)s) / KZ_FIXED_ONE;
}

static kz_status_t check_rect(const kz_rect_t *r) {
    if (!r) return KZ_EINVAL;
    if (r->w < 0 || r->h < 0) return KZ_EINVAL;
    return KZ_OK;
}

static int rect_isect(int64_t ax, int64_t ay, int64_t aw, int64_t ah,
                      int64_t bx, int64_t by, int64_t bw, int64_t bh,
                      int64_t *ox, int64_t *oy, int64_t *ow, int64_t *oh) {
    int64_t x0 = ax > bx ? ax : bx;
    int64_t y0 = ay > by ? ay : by;
    int64_t x1 = (ax + aw) < (bx + bw) ? (ax + aw) : (bx + bw);
    int64_t y1 = (ay + ah) < (by + bh) ? (ay + ah) : (by + bh);
    *ox = x0;
    *oy = y0;
    *ow = x1 - x0;
    *oh = y1 - y0;
    return (*ow > 0 && *oh > 0);
}

static uint32_t blend_pixel(uint32_t dst, uint32_t src_rgb, uint32_t alpha,
                             uint32_t format) {
    uint32_t sr = (src_rgb >> 16) & 0xFF;
    uint32_t sg = (src_rgb >> 8) & 0xFF;
    uint32_t sb = src_rgb & 0xFF;
    if (alpha >= 255) {
        if (format == KZ_FMT_ARGB8888)
            return (0xFFu << 24) | (sr << 16) | (sg << 8) | sb;
        return (sr << 16) | (sg << 8) | sb;
    }
    uint32_t na = 255u - alpha;
    if (format == KZ_FMT_ARGB8888) {
        uint32_t da = (dst >> 24) & 0xFF;
        uint32_t dr = (dst >> 16) & 0xFF;
        uint32_t dg = (dst >> 8) & 0xFF;
        uint32_t db = dst & 0xFF;
        uint32_t a = alpha + (da * na + 127u) / 255u;
        uint32_t r = (sr * alpha + dr * na + 127u) / 255u;
        uint32_t g = (sg * alpha + dg * na + 127u) / 255u;
        uint32_t b = (sb * alpha + db * na + 127u) / 255u;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = dst & 0xFF;
    uint32_t r = (sr * alpha + dr * na + 127u) / 255u;
    uint32_t g = (sg * alpha + dg * na + 127u) / 255u;
    uint32_t b = (sb * alpha + db * na + 127u) / 255u;
    return (r << 16) | (g << 8) | b;
}

static uint32_t blit_pixel(uint32_t dst, uint32_t src, int src_argb,
                            uint32_t alpha, uint32_t dst_fmt) {
    uint32_t a = alpha;
    if (src_argb) {
        uint32_t sa = (src >> 24) & 0xFF;
        a = (sa * alpha + 127u) / 255u;
    }
    return blend_pixel(dst, src, a, dst_fmt);
}

static void fill_span(struct kz_context *c, int64_t x, int64_t y,
                      int64_t w, uint32_t color, uint32_t alpha) {
    uint32_t *row = c->pixels + y * (int64_t)c->stride_px + x;
    if (alpha >= 255) {
        uint32_t px = (c->format == KZ_FMT_ARGB8888)
                          ? ((0xFFu << 24) | (color & 0xFFFFFFu))
                          : (color & 0xFFFFFFu);
        for (int64_t i = 0; i < w; i++) row[i] = px;
        return;
    }
    for (int64_t i = 0; i < w; i++)
        row[i] = blend_pixel(row[i], color & 0xFFFFFFu, alpha, c->format);
}

kz_status_t kz_target_wrap(uint32_t *pixels, uint32_t width, uint32_t height,
                            uint32_t stride_px, uint32_t format,
                            kz_surface_desc_t *out_desc) {
    kz_surface_desc_t d;
    if (!pixels || !out_desc) return KZ_EINVAL;
    if (format != KZ_FMT_XRGB8888 && format != KZ_FMT_ARGB8888) return KZ_EINVAL;
    if (width == 0 || height == 0) return KZ_EINVAL;
    if (width > KZ_SURFACE_MAX_DIM || height > KZ_SURFACE_MAX_DIM) return KZ_EINVAL;
    if (stride_px < width) return KZ_EINVAL;
    uint64_t need = (uint64_t)stride_px * height * 4u;
    if (height != 0 && need / height / 4u != stride_px) return KZ_ERANGE;
    if (need > KZ_SURFACE_MAX_BYTES) return KZ_EINVAL;
    d.width = width;
    d.height = height;
    d.stride_px = stride_px;
    d.format = format;
    d.size_bytes = need;
    d.usage = KZ_USAGE_CPU_READ | KZ_USAGE_CPU_WRITE;
    d.id = (uint64_t)(uintptr_t)pixels;
    d.generation = 0;
    d.owner = 0;
    if (kz_surface_validate(&d) != KZ_OK) return KZ_EINVAL;
    *out_desc = d;
    return KZ_OK;
}

kz_status_t kz_target_create(uint32_t width, uint32_t height, uint32_t format,
                              kz_alloc_fn alloc, kz_free_fn free_fn,
                              kz_surface_desc_t *out_desc,
                              uint32_t **out_pixels) {
    uint64_t need;
    uint32_t *pixels;
    kz_status_t st;
    if (!out_desc || !out_pixels) return KZ_EINVAL;
    if (!alloc || !free_fn) return KZ_EINVAL;
    if (format != KZ_FMT_XRGB8888 && format != KZ_FMT_ARGB8888) return KZ_EINVAL;
    if (width == 0 || height == 0) return KZ_EINVAL;
    if (width > KZ_SURFACE_MAX_DIM || height > KZ_SURFACE_MAX_DIM) return KZ_EINVAL;
    need = kz_surface_bytes(width, height);
    if (need == 0 || need > KZ_SURFACE_MAX_BYTES) return KZ_EINVAL;
    if (need > 0xFFFFFFFFu) return KZ_EINVAL;
    pixels = (uint32_t *)alloc((uint32_t)need);
    if (!pixels) return KZ_ENOMEM;
    memset(pixels, 0, (size_t)need);
    st = kz_target_wrap(pixels, width, height, width, format, out_desc);
    if (st != KZ_OK) {
        free_fn(pixels);
        return st;
    }
    *out_pixels = pixels;
    return KZ_OK;
}

void kz_target_free(const kz_surface_desc_t *desc, uint32_t *pixels,
                    kz_free_fn free_fn) {
    (void)desc;
    if (pixels && free_fn) free_fn(pixels);
}

kz_status_t kz_context_create(const kz_surface_desc_t *target,
                               uint32_t *pixels,
                               kz_alloc_fn alloc, kz_free_fn free_fn,
                               kz_context_t **out_ctx) {
    kz_context_t *c;
    if (!target || !pixels || !out_ctx) return KZ_EINVAL;
    if (!alloc || !free_fn) return KZ_EINVAL;
    if (kz_surface_validate(target) != KZ_OK) return KZ_EINVAL;
    if (target->width > (uint32_t)0x7FFFFFFF ||
        target->height > (uint32_t)0x7FFFFFFF)
        return KZ_EINVAL;
    c = (kz_context_t *)alloc((uint32_t)sizeof(*c));
    if (!c) return KZ_ENOMEM;
    c->pixels = pixels;
    c->width = target->width;
    c->height = target->height;
    c->stride_px = target->stride_px;
    c->format = target->format;
    c->tr.sx = KZ_FIXED_ONE;
    c->tr.sy = KZ_FIXED_ONE;
    c->tr.tx = 0;
    c->tr.ty = 0;
    c->clip.x = 0;
    c->clip.y = 0;
    c->clip.w = (int32_t)target->width;
    c->clip.h = (int32_t)target->height;
    c->depth = 0;
    *out_ctx = c;
    return KZ_OK;
}

void kz_context_destroy(kz_context_t *ctx, kz_free_fn free_fn) {
    if (ctx && free_fn) free_fn(ctx);
}

kz_status_t kz_save(kz_context_t *ctx) {
    if (!ctx) return KZ_EINVAL;
    if (ctx->depth >= KZ_STATE_MAX) return KZ_ESTATE;
    ctx->tr_stack[ctx->depth] = ctx->tr;
    ctx->clip_stack[ctx->depth] = ctx->clip;
    ctx->depth++;
    return KZ_OK;
}

kz_status_t kz_restore(kz_context_t *ctx) {
    if (!ctx) return KZ_EINVAL;
    if (ctx->depth == 0) return KZ_ESTATE;
    ctx->depth--;
    ctx->tr = ctx->tr_stack[ctx->depth];
    ctx->clip = ctx->clip_stack[ctx->depth];
    return KZ_OK;
}

kz_status_t kz_set_transform(kz_context_t *ctx, const kz_transform_t *t) {
    if (!ctx || !t) return KZ_EINVAL;
    if (t->sx <= 0 || t->sy <= 0) return KZ_EINVAL;
    ctx->tr = *t;
    return KZ_OK;
}

kz_status_t kz_clip_rect(kz_context_t *ctx, const kz_rect_t *r) {
    int64_t x, y, w, h;
    kz_status_t st;
    if (!ctx) return KZ_EINVAL;
    st = check_rect(r);
    if (st != KZ_OK) return st;
    if (!rect_isect(ctx->clip.x, ctx->clip.y, ctx->clip.w, ctx->clip.h,
                     r->x, r->y, r->w, r->h, &x, &y, &w, &h)) {
        ctx->clip.w = 0;
        ctx->clip.h = 0;
        return KZ_OK;
    }
    ctx->clip.x = (int32_t)x;
    ctx->clip.y = (int32_t)y;
    ctx->clip.w = (int32_t)w;
    ctx->clip.h = (int32_t)h;
    return KZ_OK;
}

static kz_status_t fill_mapped(struct kz_context *c, int64_t tx, int64_t ty,
                                int64_t tw, int64_t th, uint32_t color,
                                uint32_t alpha) {
    int64_t x, y, w, h;
    int64_t i;
    if (tw <= 0 || th <= 0) return KZ_OK;
    if (!rect_isect(tx, ty, tw, th, c->clip.x, c->clip.y, c->clip.w, c->clip.h,
                     &x, &y, &w, &h))
        return KZ_OK;
    if (!rect_isect(x, y, w, h, 0, 0, c->width, c->height, &x, &y, &w, &h))
        return KZ_OK;
    for (i = 0; i < h; i++) fill_span(c, x, y + i, w, color, alpha);
    return KZ_OK;
}

kz_status_t kz_fill_rect(kz_context_t *ctx, const kz_rect_t *r,
                          const kz_paint_t *paint) {
    int64_t tx, ty, tw, th;
    kz_status_t st;
    uint32_t alpha;
    if (!ctx) return KZ_EINVAL;
    st = check_rect(r);
    if (st != KZ_OK) return st;
    if (!paint) return KZ_EINVAL;
    if (r->w == 0 || r->h == 0) return KZ_OK;
    alpha = paint->alpha > 255 ? 255 : paint->alpha;
    tx = mul1024(r->x, ctx->tr.sx) + ctx->tr.tx;
    ty = mul1024(r->y, ctx->tr.sy) + ctx->tr.ty;
    tw = mul1024(r->w, ctx->tr.sx);
    th = mul1024(r->h, ctx->tr.sy);
    return fill_mapped(ctx, tx, ty, tw, th, paint->color, alpha);
}

kz_status_t kz_draw_rect(kz_context_t *ctx, const kz_rect_t *r,
                          const kz_paint_t *paint) {
    kz_rect_t edge;
    kz_status_t st;
    if (!ctx) return KZ_EINVAL;
    st = check_rect(r);
    if (st != KZ_OK) return st;
    if (!paint) return KZ_EINVAL;
    if (r->w == 0 || r->h == 0) return KZ_OK;
    edge.x = r->x;
    edge.y = r->y;
    edge.w = r->w;
    edge.h = 1;
    st = kz_fill_rect(ctx, &edge, paint);
    if (st != KZ_OK) return st;
    if (r->h > 1) {
        edge.y = r->y + r->h - 1;
        st = kz_fill_rect(ctx, &edge, paint);
        if (st != KZ_OK) return st;
    }
    if (r->h > 2 && r->w > 0) {
        edge.x = r->x;
        edge.y = r->y + 1;
        edge.w = 1;
        edge.h = r->h - 2;
        st = kz_fill_rect(ctx, &edge, paint);
        if (st != KZ_OK) return st;
        if (r->w > 1) {
            edge.x = r->x + r->w - 1;
            st = kz_fill_rect(ctx, &edge, paint);
            if (st != KZ_OK) return st;
        }
    }
    return KZ_OK;
}

kz_status_t kz_clear(kz_context_t *ctx, uint32_t color) {
    int64_t y;
    uint32_t px;
    if (!ctx) return KZ_EINVAL;
    px = (ctx->format == KZ_FMT_ARGB8888) ? ((0xFFu << 24) | (color & 0xFFFFFFu))
                                          : (color & 0xFFFFFFu);
    for (y = 0; y < (int64_t)ctx->height; y++) {
        uint32_t *row = ctx->pixels + y * (int64_t)ctx->stride_px;
        int64_t x;
        for (x = 0; x < (int64_t)ctx->width; x++) row[x] = px;
    }
    return KZ_OK;
}

kz_status_t kz_blit(kz_context_t *ctx, const uint32_t *src_pixels,
                     const kz_surface_desc_t *src, const kz_rect_t *src_rect,
                     const kz_rect_t *dst_rect, const kz_paint_t *paint) {
    int64_t dx, dy, dw, dh;
    int64_t sx, sy;
    int64_t x, y, w, h;
    int64_t i;
    uint32_t alpha;
    kz_status_t st;
    int same;
    int down;
    int src_argb;
    if (!ctx || !src_pixels || !src) return KZ_EINVAL;
    st = check_rect(src_rect);
    if (st != KZ_OK) return st;
    st = check_rect(dst_rect);
    if (st != KZ_OK) return st;
    if (!paint) return KZ_EINVAL;
    if (kz_surface_validate(src) != KZ_OK) return KZ_EINVAL;
    if (src->format != ctx->format) return KZ_ENOSUP;
    if (src_rect->x < 0 || src_rect->y < 0) return KZ_EINVAL;
    if ((uint64_t)src_rect->x + (uint64_t)src_rect->w > src->width ||
        (uint64_t)src_rect->y + (uint64_t)src_rect->h > src->height)
        return KZ_EINVAL;
    if (src_rect->w != dst_rect->w || src_rect->h != dst_rect->h) return KZ_ENOSUP;
    if (src_rect->w == 0 || src_rect->h == 0) return KZ_OK;
    alpha = paint->alpha > 255 ? 255 : paint->alpha;
    dx = mul1024(dst_rect->x, ctx->tr.sx) + ctx->tr.tx;
    dy = mul1024(dst_rect->y, ctx->tr.sy) + ctx->tr.ty;
    dw = mul1024(dst_rect->w, ctx->tr.sx);
    dh = mul1024(dst_rect->h, ctx->tr.sy);
    if (dw != dst_rect->w || dh != dst_rect->h) return KZ_ENOSUP;
    sx = src_rect->x;
    sy = src_rect->y;
    if (!rect_isect(dx, dy, dw, dh, ctx->clip.x, ctx->clip.y, ctx->clip.w,
                     ctx->clip.h, &x, &y, &w, &h))
        return KZ_OK;
    if (!rect_isect(x, y, w, h, 0, 0, ctx->width, ctx->height, &x, &y, &w, &h))
        return KZ_OK;
    sx += (x - dx);
    sy += (y - dy);
    same = (src_pixels == ctx->pixels && src->stride_px == ctx->stride_px);
    down = same && (y > sy);
    src_argb = (src->format == KZ_FMT_ARGB8888);
    for (i = 0; i < h; i++) {
        int64_t r = down ? (h - 1 - i) : i;
        const uint32_t *srow =
            src_pixels + (sy + r) * (int64_t)src->stride_px + sx;
        uint32_t *drow = ctx->pixels + (y + r) * (int64_t)ctx->stride_px + x;
        if (alpha >= 255 && !src_argb) {
            int64_t k;
            if (same) {
                memmove(drow, srow, (size_t)(w * 4));
            } else if (ctx->format == KZ_FMT_XRGB8888) {
                for (k = 0; k < w; k++) drow[k] = srow[k] & 0xFFFFFFu;
            } else if (src->format == KZ_FMT_XRGB8888) {
                for (k = 0; k < w; k++)
                    drow[k] = (0xFFu << 24) | (srow[k] & 0xFFFFFFu);
            } else {
                for (k = 0; k < w; k++) drow[k] = srow[k];
            }
        } else {
            int64_t k;
            int back = same && (y + r == sy + r) && (x > sx);
            for (k = 0; k < w; k++) {
                int64_t j = back ? (w - 1 - k) : k;
                uint32_t s = ((const uint32_t *)srow)[j];
                drow[j] = blit_pixel(drow[j], s, src_argb, alpha,
                                     ctx->format);
            }
        }
    }
    return KZ_OK;
}

kz_status_t kz_flush(kz_context_t *ctx, kz_fence_t **out_fence) {
    if (!ctx) return KZ_EINVAL;
    if (out_fence) *out_fence = NULL;
    return KZ_OK;
}
