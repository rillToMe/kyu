#include "kz_shadow.h"

#include <stddef.h>

static int rect_empty(int64_t w, int64_t h) {
    return (w <= 0 || h <= 0);
}

static int isect(int64_t ax, int64_t ay, int64_t aw, int64_t ah,
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
    return !rect_empty(*ow, *oh);
}

kz_status_t kz_shadow_compose(kz_context_t *target,
                              const kz_surface_desc_t *target_desc,
                              const kz_layer_t *layers, uint32_t n_layers,
                              const kz_rect_t *damage, uint32_t n_damage,
                              uint32_t background,
                              kz_shadow_stats_t *out_stats) {
    kz_rect_t rects[KZ_SHADOW_MAX_DAMAGE + 1];
    uint32_t nrects = 0;
    uint32_t order[KZ_SHADOW_MAX_LAYERS];
    uint32_t i, j;
    kz_transform_t ident = {KZ_FIXED_ONE, KZ_FIXED_ONE, 0, 0};
    kz_paint_t bg;
    kz_status_t st;
    int64_t tw, th;
    uint32_t drawn = 0, skipped = 0;
    uint32_t cleared = 0, blended = 0;
    kz_rect_t full;

    if (!target || !target_desc) return KZ_EINVAL;
    if (kz_surface_validate(target_desc) != KZ_OK) return KZ_EINVAL;
    if (n_layers > KZ_SHADOW_MAX_LAYERS) return KZ_EINVAL;
    if (n_layers > 0 && !layers) return KZ_EINVAL;
    if (n_damage > 0 && !damage) return KZ_EINVAL;
    tw = target_desc->width;
    th = target_desc->height;
    if (tw > 0x7FFFFFFF || th > 0x7FFFFFFF) return KZ_EINVAL;

    for (i = 0; i < n_layers; i++) {
        const kz_layer_t *l = &layers[i];
        if (!l->visible || l->opacity == 0) {
            skipped++;
            continue;
        }
        if (!l->pixels) {
            if (out_stats) {
                *out_stats = (kz_shadow_stats_t){0, 0, 0, 0, 0, i};
            }
            return KZ_EINVAL;
        }
        if (kz_surface_validate(&l->desc) != KZ_OK) {
            if (out_stats) {
                *out_stats = (kz_shadow_stats_t){0, 0, 0, 0, 0, i};
            }
            return KZ_EINVAL;
        }
        if (l->desc.format != target_desc->format) {
            if (out_stats) {
                *out_stats = (kz_shadow_stats_t){0, 0, 0, 0, 0, i};
            }
            return KZ_ENOSUP;
        }
        if (l->opacity > 255) {
            if (out_stats) {
                *out_stats = (kz_shadow_stats_t){0, 0, 0, 0, 0, i};
            }
            return KZ_EINVAL;
        }
    }

    if (n_damage == 0) {
        rects[0].x = 0;
        rects[0].y = 0;
        rects[0].w = (int32_t)tw;
        rects[0].h = (int32_t)th;
        nrects = 1;
    } else {
        int64_t bx0 = tw, by0 = th, bx1 = 0, by1 = 0;
        for (i = 0; i < n_damage; i++) {
            int64_t x, y, w, h;
            if (damage[i].w < 0 || damage[i].h < 0) {
                if (out_stats) {
                    *out_stats = (kz_shadow_stats_t){0, 0, 0, 0, 0,
                                                     KZ_SHADOW_NO_FAIL};
                }
                return KZ_EINVAL;
            }
            if (damage[i].w == 0 || damage[i].h == 0) continue;
            if (!isect(damage[i].x, damage[i].y, damage[i].w, damage[i].h,
                        0, 0, tw, th, &x, &y, &w, &h))
                continue;
            if (nrects < KZ_SHADOW_MAX_DAMAGE) {
                rects[nrects].x = (int32_t)x;
                rects[nrects].y = (int32_t)y;
                rects[nrects].w = (int32_t)w;
                rects[nrects].h = (int32_t)h;
                nrects++;
            }
            if (x < bx0) bx0 = x;
            if (y < by0) by0 = y;
            if (x + w > bx1) bx1 = x + w;
            if (y + h > by1) by1 = y + h;
        }
        if (n_damage > KZ_SHADOW_MAX_DAMAGE && bx1 > bx0 && by1 > by0) {
            rects[0].x = (int32_t)bx0;
            rects[0].y = (int32_t)by0;
            rects[0].w = (int32_t)(bx1 - bx0);
            rects[0].h = (int32_t)(by1 - by0);
            nrects = 1;
        }
        if (nrects == 0) {
            if (out_stats) {
                *out_stats = (kz_shadow_stats_t){0, 0, 0, 0, 0,
                                                 KZ_SHADOW_NO_FAIL};
            }
            return KZ_OK;
        }
    }

    for (i = 0; i < n_layers; i++) order[i] = i;
    for (i = 1; i < n_layers; i++) {
        uint32_t key = order[i];
        j = i;
        while (j > 0 && layers[order[j - 1]].z > layers[key].z) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    st = kz_save(target);
    if (st != KZ_OK) return st;
    st = kz_set_transform(target, &ident);
    if (st != KZ_OK) {
        kz_restore(target);
        return st;
    }
    full.x = 0;
    full.y = 0;
    full.w = (int32_t)tw;
    full.h = (int32_t)th;
    st = kz_clip_rect(target, &full);
    if (st != KZ_OK) {
        kz_restore(target);
        return st;
    }
    bg.color = background;
    bg.alpha = 255;

    for (i = 0; i < nrects; i++) {
        uint32_t k;
        st = kz_save(target);
        if (st != KZ_OK) break;
        st = kz_clip_rect(target, &rects[i]);
        if (st != KZ_OK) {
            kz_restore(target);
            break;
        }
        st = kz_fill_rect(target, &rects[i], &bg);
        if (st != KZ_OK) {
            kz_restore(target);
            break;
        }
        cleared += (uint32_t)((uint64_t)(uint32_t)rects[i].w * (uint32_t)rects[i].h);
        for (k = 0; k < n_layers; k++) {
            const kz_layer_t *l = &layers[order[k]];
            int64_t lx = l->x, ly = l->y;
            int64_t lw = l->desc.width, lh = l->desc.height;
            int64_t x, y, w, h;
            kz_rect_t sr, dr;
            kz_paint_t p;
            if (!l->visible || l->opacity == 0) continue;
            if (!isect(lx, ly, lw, lh, rects[i].x, rects[i].y, rects[i].w,
                        rects[i].h, &x, &y, &w, &h))
                continue;
            sr.x = (int32_t)(x - lx);
            sr.y = (int32_t)(y - ly);
            sr.w = (int32_t)w;
            sr.h = (int32_t)h;
            dr.x = (int32_t)x;
            dr.y = (int32_t)y;
            dr.w = (int32_t)w;
            dr.h = (int32_t)h;
            p.color = 0;
            p.alpha = l->opacity;
            st = kz_blit(target, l->pixels, &l->desc, &sr, &dr, &p);
            if (st != KZ_OK) break;
            drawn++;
            blended += (uint32_t)((uint64_t)(uint32_t)w * (uint32_t)h);
        }
        {
            kz_status_t rs = kz_restore(target);
            if (st == KZ_OK) st = rs;
        }
        if (st != KZ_OK) {
            if (out_stats) {
                *out_stats = (kz_shadow_stats_t){0, 0, 0, 0, 0, k};
            }
            kz_restore(target);
            return st;
        }
    }
    {
        kz_status_t rs = kz_restore(target);
        if (st == KZ_OK) st = rs;
    }
    if (st != KZ_OK) return st;
    if (out_stats) {
        out_stats->damage_rects = nrects;
        out_stats->pixels_cleared = cleared;
        out_stats->pixels_blended = blended;
        out_stats->layers_drawn = drawn;
        out_stats->layers_skipped = skipped;
        out_stats->failed_index = KZ_SHADOW_NO_FAIL;
    }
    return KZ_OK;
}
