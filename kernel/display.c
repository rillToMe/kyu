#include "display.h"
#include "heap.h"
#include "string.h"

static uint8_t rect_clip_to_buffer(const DisplayBuffer* buffer, Rect* area) {
    int64_t x0 = area->x;
    int64_t y0 = area->y;
    int64_t x1 = x0 + (int64_t)area->width;
    int64_t y1 = y0 + (int64_t)area->height;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)buffer->width)  x1 = buffer->width;
    if (y1 > (int64_t)buffer->height) y1 = buffer->height;

    if (x1 <= x0 || y1 <= y0) return 0;

    area->x = (int32_t)x0;
    area->y = (int32_t)y0;
    area->width  = (uint32_t)(x1 - x0);
    area->height = (uint32_t)(y1 - y0);
    return 1;
}

DisplayBuffer* display_buffer_create(uint32_t width, uint32_t height, ColorFormat format) {
    if (width == 0 || height == 0) return NULL;

    DisplayBuffer* buffer = (DisplayBuffer*)kmalloc(sizeof(DisplayBuffer));
    if (!buffer) return NULL;

    // Bug 5.6: width*height*4 bisa overflow walau dihitung 64-bit. Hitung di
    // uint64_t dan tolak hasil yang melewati SIZE_MAX (dipakai kmalloc).
    uint64_t bytes = (uint64_t)width * (uint64_t)height * sizeof(uint32_t);
    if (width != 0 && bytes / width / sizeof(uint32_t) != height) {
        kfree(buffer);
        return NULL;   // overflow — tolak
    }
    if (bytes > (uint64_t)SIZE_MAX) {
        kfree(buffer);
        return NULL;
    }
    buffer->pixels = (uint32_t*)kmalloc((size_t)bytes);
    if (!buffer->pixels) {
        kfree(buffer);
        return NULL;
    }

    buffer->width = width;
    buffer->height = height;
    buffer->stride = width;
    buffer->format = format;
    buffer->owns_pixels = 1;
    buffer->dirty = NULL;
    return buffer;
}

DisplayBuffer* display_buffer_wrap(uint32_t* pixels, uint32_t width, uint32_t height,
                                   uint32_t stride, ColorFormat format) {
    if (!pixels || width == 0 || height == 0 || stride < width) return NULL;

    DisplayBuffer* buffer = (DisplayBuffer*)kmalloc(sizeof(DisplayBuffer));
    if (!buffer) return NULL;

    buffer->pixels = pixels;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->format = format;
    buffer->owns_pixels = 0;
    buffer->dirty = NULL;
    return buffer;
}

void display_buffer_destroy(DisplayBuffer* buffer) {
    if (!buffer) return;
    if (buffer->owns_pixels) kfree(buffer->pixels);
    kfree(buffer);
}

void display_buffer_write_pixel(DisplayBuffer* buffer, int32_t x, int32_t y, Color color) {
    if (!buffer) return;
    if (x < 0 || y < 0 || (uint32_t)x >= buffer->width || (uint32_t)y >= buffer->height) return;
    buffer->pixels[(uint32_t)y * buffer->stride + (uint32_t)x] = color;
    if (buffer->dirty) {
        Rect r = { x, y, 1, 1 };
        dirty_region_mark(buffer->dirty, r);
    }
}

void display_buffer_fill_rect(DisplayBuffer* buffer, Rect area, Color color) {
    if (!buffer) return;
    if (!rect_clip_to_buffer(buffer, &area)) return;

    for (uint32_t row = 0; row < area.height; row++) {
        uint32_t* line = buffer->pixels + ((uint32_t)area.y + row) * buffer->stride + (uint32_t)area.x;
        for (uint32_t col = 0; col < area.width; col++) {
            line[col] = color;
        }
    }
    if (buffer->dirty) dirty_region_mark(buffer->dirty, area);
}

int rect_intersect(Rect a, Rect b, Rect* out) {
    int64_t x0 = a.x > b.x ? a.x : b.x;
    int64_t y0 = a.y > b.y ? a.y : b.y;
    int64_t ax1 = (int64_t)a.x + a.width,  ay1 = (int64_t)a.y + a.height;
    int64_t bx1 = (int64_t)b.x + b.width,  by1 = (int64_t)b.y + b.height;
    int64_t x1 = ax1 < bx1 ? ax1 : bx1;
    int64_t y1 = ay1 < by1 ? ay1 : by1;

    if (x1 <= x0 || y1 <= y0) return 0;
    out->x = (int32_t)x0;
    out->y = (int32_t)y0;
    out->width  = (uint32_t)(x1 - x0);
    out->height = (uint32_t)(y1 - y0);
    return 1;
}

Rect rect_union(Rect a, Rect b) {
    int64_t x0 = a.x < b.x ? a.x : b.x;
    int64_t y0 = a.y < b.y ? a.y : b.y;
    int64_t ax1 = (int64_t)a.x + a.width,  ay1 = (int64_t)a.y + a.height;
    int64_t bx1 = (int64_t)b.x + b.width,  by1 = (int64_t)b.y + b.height;
    int64_t x1 = ax1 > bx1 ? ax1 : bx1;
    int64_t y1 = ay1 > by1 ? ay1 : by1;

    Rect out;
    out.x = (int32_t)x0;
    out.y = (int32_t)y0;
    out.width  = (uint32_t)(x1 - x0);
    out.height = (uint32_t)(y1 - y0);
    return out;
}

// ============================================================
// Phase 15 — bounded dirty-region coalescing
//
// Before: once more than MAX_DIRTY_REGIONS rects accumulated, the entire list
// was replaced by ONE bounding box (the old collapse branch). Scattered small
// updates — glyph cells, widget hovers, a few windows — then made the
// compositor base-blit + composite + upload the whole spanned area. Phase 11
// ranked this collapse the #1 work amplifier.
//
// Over-invalidation is allowed; under-invalidation is forbidden. Every
// transform below only ever grows coverage (rect_union), so
//     union(coalesced)  is a SUPERSET of  union(requested)
// always holds. That is the correctness invariant.
//
// The O(N^2) pair search runs only when the list is FULL and the new rect is
// disjoint from every region. N = MAX_DIRTY_REGIONS = 64, so even that is a few
// thousand cheap integer unions — negligible next to composing pixels.
// ============================================================

static uint64_t rect_area_u64(Rect r) {
    return (uint64_t)r.width * (uint64_t)r.height;
}

// Extra pixels a merge forces the compositor to process: bounding-box area
// minus both source areas. SIGNED: for two overlapping rects the bounding box
// can be SMALLER than their sum, so this is negative and the merge is a strict
// win (it also removes the double-processed overlap). For separated rects it is
// positive and equals the wasted gap. This is the area cost used to choose
// merges; it must be signed or the comparison wraps.
static int64_t merge_inflation(Rect a, Rect b) {
    Rect u = rect_union(a, b);
    return (int64_t)rect_area_u64(u) - (int64_t)rect_area_u64(a)
                                     - (int64_t)rect_area_u64(b);
}

// 1 if `inner` lies fully inside `outer` (64-bit safe; Rect may be negative).
static int rect_contains(Rect outer, Rect inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           (int64_t)inner.x + inner.width  <= (int64_t)outer.x + outer.width &&
           (int64_t)inner.y + inner.height <= (int64_t)outer.y + outer.height;
}

// Drop regions fully contained in `cover` (regions[keep]). Used after a region
// grows: the covered regions are now redundant. Compacts in place, O(N).
static void dirty_drop_contained(DirtyRegionList* list, Rect cover, uint32_t keep) {
    uint32_t k = 0;
    for (uint32_t j = 0; j < list->count; j++) {
        if (j != keep && rect_contains(cover, list->regions[j])) continue;
        list->regions[k++] = list->regions[j];
    }
    list->count = k;
}

void dirty_region_clear(DirtyRegionList* list) {
    if (!list) return;
    list->count = 0;
    list->collapsed = 0;
}

void dirty_region_mark(DirtyRegionList* list, Rect r) {
    if (!list || r.width == 0 || r.height == 0) return;

    // 1. Already covered by an existing region.
    for (uint32_t i = 0; i < list->count; i++)
        if (rect_contains(list->regions[i], r)) return;

    // 2. Merge into the overlapping region that costs the least, but only when
    //    the merge does not add work (inflation <= 0: the bounding box is no
    //    larger than the two regions processed separately, and any shared
    //    overlap stops being processed twice). Lightly-overlapping rects that
    //    would inflate stay separate — a bbox is a worse representation than
    //    the sum of the parts, so eagerly folding them would CREATE work.
    int     m = -1;
    int64_t m_cost = 0;
    for (uint32_t i = 0; i < list->count; i++) {
        Rect clip;
        if (!rect_intersect(list->regions[i], r, &clip)) continue;
        int64_t c = merge_inflation(list->regions[i], r);
        if (m < 0 || c < m_cost) { m = (int)i; m_cost = c; }
    }
    if (m >= 0 && m_cost <= 0) {
        list->regions[m] = rect_union(list->regions[m], r);
        dirty_drop_contained(list, list->regions[m], (uint32_t)m);
        return;
    }

    // 3. Independent (or too costly to merge): append while there is room.
    //    Regions r fully covers are superseded and dropped.
    if (list->count < MAX_DIRTY_REGIONS) {
        uint32_t k = 0;
        for (uint32_t j = 0; j < list->count; j++) {
            if (rect_contains(r, list->regions[j])) continue;
            list->regions[k++] = list->regions[j];
        }
        list->regions[k++] = r;
        list->count = k;
        return;
    }

    // 4. Full. Do NOT collapse everything into one bbox. Keep the list bounded
    //    by the least wasteful option — either grow one region to include r, or
    //    merge the cheapest existing pair and keep r separate. O(N^2) is fine:
    //    N = MAX_DIRTY_REGIONS = 64 and this runs only under pressure.
    uint32_t best = 0;
    int64_t  best_cost = merge_inflation(list->regions[0], r);
    for (uint32_t i = 1; i < list->count; i++) {
        int64_t c = merge_inflation(list->regions[i], r);
        if (c < best_cost) { best_cost = c; best = i; }
    }
    uint32_t pi = 0, pj = 1;
    int64_t  pair_cost = merge_inflation(list->regions[0], list->regions[1]);
    for (uint32_t i = 0; i < list->count; i++) {
        for (uint32_t j = i + 1; j < list->count; j++) {
            int64_t c = merge_inflation(list->regions[i], list->regions[j]);
            if (c < pair_cost) { pair_cost = c; pi = i; pj = j; }
        }
    }

    if (best_cost <= pair_cost) {
        // Growing one region to include r wastes less than merging two others.
        Rect cover = rect_union(list->regions[best], r);
        list->regions[best] = cover;
        dirty_drop_contained(list, cover, best);
        return;
    }

    // Merge the cheapest existing pair to free a slot, then place r.
    Rect cover = rect_union(list->regions[pi], list->regions[pj]);
    list->regions[pi] = cover;
    for (uint32_t k = pj; k + 1 < list->count; k++) list->regions[k] = list->regions[k + 1];
    list->count--;
    if (pi > pj) pi--;
    dirty_drop_contained(list, cover, pi);
    if (list->count < MAX_DIRTY_REGIONS && !rect_contains(cover, r))
        list->regions[list->count++] = r;
}

void viewport_scroll(Viewport* vp, int32_t dx, int32_t dy) {
    if (!vp) return;
    vp->scroll_x += dx;
    vp->scroll_y += dy;
    if (vp->scroll_x < 0) vp->scroll_x = 0;
    if (vp->scroll_y < 0) vp->scroll_y = 0;
}

// Jalur panas compositor (blit base→back & back→fb tiap frame): clipping
// dihitung SEKALI per panggilan, inner loop = memcpy per baris — bukan
// bounds check per pixel.
void viewport_render(Viewport* vp, DisplayBuffer* target) {
    if (!vp || !target || !vp->source) return;
    const DisplayBuffer* src = vp->source;

    // Rentang kolom [c0, c1): source & target sama-sama in-bounds.
    int64_t c0 = 0, c1 = (int64_t)vp->bounds.width;
    if (-(int64_t)vp->scroll_x > c0)                    c0 = -(int64_t)vp->scroll_x;
    if ((int64_t)src->width - vp->scroll_x < c1)        c1 = (int64_t)src->width - vp->scroll_x;
    if (-(int64_t)vp->bounds.x > c0)                    c0 = -(int64_t)vp->bounds.x;
    if ((int64_t)target->width - vp->bounds.x < c1)     c1 = (int64_t)target->width - vp->bounds.x;
    if (c1 <= c0) return;

    // Rentang baris [r0, r1): idem.
    int64_t r0 = 0, r1 = (int64_t)vp->bounds.height;
    if (-(int64_t)vp->scroll_y > r0)                    r0 = -(int64_t)vp->scroll_y;
    if ((int64_t)src->height - vp->scroll_y < r1)       r1 = (int64_t)src->height - vp->scroll_y;
    if (-(int64_t)vp->bounds.y > r0)                    r0 = -(int64_t)vp->bounds.y;
    if ((int64_t)target->height - vp->bounds.y < r1)    r1 = (int64_t)target->height - vp->bounds.y;
    if (r1 <= r0) return;

    uint64_t row_bytes = (uint64_t)(c1 - c0) * 4;
    for (int64_t row = r0; row < r1; row++) {
        const uint32_t* s = src->pixels +
            (uint64_t)(vp->scroll_y + row) * src->stride + (uint64_t)(vp->scroll_x + c0);
        uint32_t* d = target->pixels +
            (uint64_t)(vp->bounds.y + row) * target->stride + (uint64_t)(vp->bounds.x + c0);
        memcpy(d, s, row_bytes);
    }
}
