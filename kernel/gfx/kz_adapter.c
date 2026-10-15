#include "kz_adapter.h"
#include "kz_shm.h"
#include "ghal.h"
#include "heap.h"
#include "spinlock.h"

extern uint64_t hhdm_offset;

_Static_assert((int)KZ_FMT_XRGB8888 == (int)GHAL_FMT_XRGB8888,
               "format mirror XRGB");
_Static_assert((int)KZ_FMT_ARGB8888 == (int)GHAL_FMT_ARGB8888,
               "format mirror ARGB");

typedef struct {
    uint8_t used;
    uint16_t generation;
    kz_surface_desc_t desc;
    kz_surface_state_t state;
    ghal_surface_t *surface;
    uint64_t fence;
} kz_slot_t;

typedef struct {
    uint8_t used;
    ghal_surface_t *surface;
    uint64_t fence;
} kz_quarantine_t;

static kz_slot_t g_slots[KZ_ADAPTER_MAX_RES];
static kz_quarantine_t g_quarantine[KZ_ADAPTER_MAX_QUARANTINE];
static uint16_t g_next_gen = 1;
static spinlock_t g_adapter_lock = SPINLOCK_INIT;

static kz_handle_t make_handle(uint32_t index, uint16_t gen) {
    return ((uint32_t)gen << 16) | (index + 1);
}

static int split_handle(kz_handle_t h, uint32_t *index, uint16_t *gen) {
    uint32_t idx;
    if (h == 0) return -1;
    idx = (h & 0xFFFFu) - 1;
    if (idx >= KZ_ADAPTER_MAX_RES) return -1;
    *index = idx;
    *gen = (uint16_t)(h >> 16);
    if (*gen == 0) return -1;
    return 0;
}

static ghal_format_t map_format(uint32_t fmt) {
    return fmt == KZ_FMT_ARGB8888 ? GHAL_FMT_ARGB8888 : GHAL_FMT_XRGB8888;
}

static void retire_quarantine(void) {
    uint32_t i;
    for (i = 0; i < KZ_ADAPTER_MAX_QUARANTINE; i++) {
        if (g_quarantine[i].used &&
            !ghal_fence_pending(g_quarantine[i].fence)) {
            ghal_surface_destroy(g_quarantine[i].surface);
            g_quarantine[i].used = 0;
            g_quarantine[i].surface = NULL;
        }
    }
}

static int quarantine_put(ghal_surface_t *s, uint64_t fence) {
    uint32_t i;
    retire_quarantine();
    for (i = 0; i < KZ_ADAPTER_MAX_QUARANTINE; i++) {
        if (!g_quarantine[i].used) {
            g_quarantine[i].used = 1;
            g_quarantine[i].surface = s;
            g_quarantine[i].fence = fence;
            return 0;
        }
    }
    return -1;
}

static kz_status_t check_rect_in(const kz_surface_desc_t *d,
                                  const kz_rect_t *r) {
    uint64_t xe, ye;
    if (!r) return KZ_EINVAL;
    if (r->w < 0 || r->h < 0) return KZ_EINVAL;
    if (r->x < 0 || r->y < 0) return KZ_EINVAL;
    if (r->w == 0 || r->h == 0) return KZ_EINVAL;
    xe = (uint64_t)r->x + (uint64_t)r->w;
    ye = (uint64_t)r->y + (uint64_t)r->h;
    if (xe > d->width || ye > d->height) return KZ_EINVAL;
    return KZ_OK;
}

kz_status_t kz_adapter_create(const kz_surface_desc_t *desc,
                               kz_handle_t *out) {
    uint32_t i;
    ghal_surface_t *s;
    uint16_t gen;
    if (!desc || !out) return KZ_EINVAL;
    if (kz_surface_validate(desc) != KZ_OK) return KZ_EINVAL;
    if (desc->format != KZ_FMT_XRGB8888 && desc->format != KZ_FMT_ARGB8888)
        return KZ_EINVAL;

    spinlock_lock(&g_adapter_lock);
    retire_quarantine();
    for (i = 0; i < KZ_ADAPTER_MAX_RES; i++) {
        if (!g_slots[i].used) break;
    }
    if (i >= KZ_ADAPTER_MAX_RES) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_EBUSY;
    }
    gen = g_next_gen;
    if (gen == 0) gen = 1;
    g_next_gen = gen + 1;
    if (g_next_gen == 0) g_next_gen = 1;
    g_slots[i].used = 1;
    g_slots[i].generation = gen;
    g_slots[i].desc = *desc;
    g_slots[i].state = KZ_SURF_CREATED;
    g_slots[i].surface = NULL;
    g_slots[i].fence = 0;
    spinlock_unlock(&g_adapter_lock);

    s = ghal_surface_create(desc->width, desc->height, map_format(desc->format));
    if (!s) {
        spinlock_lock(&g_adapter_lock);
        g_slots[i].used = 0;
        spinlock_unlock(&g_adapter_lock);
        return KZ_ENOMEM;
    }
    spinlock_lock(&g_adapter_lock);
    g_slots[i].surface = s;
    *out = make_handle(i, gen);
    spinlock_unlock(&g_adapter_lock);
    return KZ_OK;
}

kz_status_t kz_adapter_acquire(kz_handle_t h) {
    uint32_t index;
    uint16_t gen;
    kz_status_t st;
    if (split_handle(h, &index, &gen) != 0) return KZ_EINVAL;
    spinlock_lock(&g_adapter_lock);
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTALE;
    }
    st = kz_surface_transition(g_slots[index].state, KZ_SURF_WRITABLE);
    if (st == KZ_OK) g_slots[index].state = KZ_SURF_WRITABLE;
    spinlock_unlock(&g_adapter_lock);
    return st;
}

kz_status_t kz_adapter_upload(kz_handle_t h, const uint32_t *pixels,
                               uint32_t src_pitch_px, const kz_rect_t *rect) {
    uint32_t index;
    uint16_t gen;
    ghal_surface_t *s;
    ghal_rect_t gr;
    kz_status_t st;
    if (!pixels) return KZ_EINVAL;
    if (src_pitch_px == 0) return KZ_EINVAL;
    if (split_handle(h, &index, &gen) != 0) return KZ_EINVAL;
    spinlock_lock(&g_adapter_lock);
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTALE;
    }
    if (g_slots[index].state != KZ_SURF_WRITABLE) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTATE;
    }
    st = check_rect_in(&g_slots[index].desc, rect);
    if (st != KZ_OK) {
        spinlock_unlock(&g_adapter_lock);
        return st;
    }
    if ((uint64_t)rect->x >= src_pitch_px) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_EINVAL;
    }
    s = g_slots[index].surface;
    gr.x = (uint32_t)rect->x;
    gr.y = (uint32_t)rect->y;
    gr.w = (uint32_t)rect->w;
    gr.h = (uint32_t)rect->h;
    spinlock_unlock(&g_adapter_lock);

    ghal_surface_upload(s, pixels, src_pitch_px, gr);

    spinlock_lock(&g_adapter_lock);
    if (g_slots[index].used && g_slots[index].generation == gen &&
        g_slots[index].state == KZ_SURF_WRITABLE)
        g_slots[index].state = KZ_SURF_READY;
    spinlock_unlock(&g_adapter_lock);
    return KZ_OK;
}

kz_status_t kz_adapter_present(kz_handle_t h, const kz_rect_t *rect) {
    uint32_t index;
    uint16_t gen;
    ghal_surface_t *s;
    const ghal_rect_t *pr = NULL;
    ghal_rect_t full;
    int accepted;
    uint64_t fence;
    if (split_handle(h, &index, &gen) != 0) return KZ_EINVAL;
    spinlock_lock(&g_adapter_lock);
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTALE;
    }
    if (g_slots[index].state != KZ_SURF_READY) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTATE;
    }
    if (rect) {
        kz_status_t st = check_rect_in(&g_slots[index].desc, rect);
        if (st != KZ_OK) {
            spinlock_unlock(&g_adapter_lock);
            return st;
        }
        full.x = (uint32_t)rect->x;
        full.y = (uint32_t)rect->y;
        full.w = (uint32_t)rect->w;
        full.h = (uint32_t)rect->h;
        pr = &full;
    }
    s = g_slots[index].surface;
    spinlock_unlock(&g_adapter_lock);

    accepted = ghal_present_checked(s, pr);

    spinlock_lock(&g_adapter_lock);
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTALE;
    }
    if (accepted != 0) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_EBUSY;
    }
    fence = ghal_present_fence();
    g_slots[index].state = KZ_SURF_IN_USE;
    g_slots[index].fence = fence;
    spinlock_unlock(&g_adapter_lock);
    return KZ_OK;
}

kz_status_t kz_adapter_poll(kz_handle_t h) {
    uint32_t index;
    uint16_t gen;
    uint64_t fence;
    int pending;
    if (split_handle(h, &index, &gen) != 0) return KZ_EINVAL;
    spinlock_lock(&g_adapter_lock);
    retire_quarantine();
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTALE;
    }
    if (g_slots[index].state != KZ_SURF_IN_USE) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTATE;
    }
    fence = g_slots[index].fence;
    spinlock_unlock(&g_adapter_lock);

    pending = ghal_fence_pending(fence);

    spinlock_lock(&g_adapter_lock);
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTALE;
    }
    if (pending) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_EBUSY;
    }
    g_slots[index].state = KZ_SURF_RELEASED;
    spinlock_unlock(&g_adapter_lock);
    return KZ_OK;
}

kz_status_t kz_adapter_destroy(kz_handle_t h) {
    uint32_t index;
    uint16_t gen;
    ghal_surface_t *s;
    uint64_t fence;
    int in_flight;
    if (split_handle(h, &index, &gen) != 0) return KZ_EINVAL;
    spinlock_lock(&g_adapter_lock);
    retire_quarantine();
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTALE;
    }
    s = g_slots[index].surface;
    in_flight = (g_slots[index].state == KZ_SURF_IN_USE);
    fence = g_slots[index].fence;
    if (in_flight && s && ghal_fence_pending(fence)) {
        if (quarantine_put(s, fence) != 0) {
            spinlock_unlock(&g_adapter_lock);
            return KZ_EBUSY;
        }
        g_slots[index].used = 0;
        g_slots[index].surface = NULL;
        spinlock_unlock(&g_adapter_lock);
        return KZ_OK;
    }
    g_slots[index].used = 0;
    g_slots[index].surface = NULL;
    spinlock_unlock(&g_adapter_lock);
    if (s) ghal_surface_destroy(s);
    return KZ_OK;
}

kz_status_t kz_adapter_resize(kz_handle_t h, const kz_surface_desc_t *desc,
                               kz_handle_t *out_new) {
    uint32_t index;
    uint16_t gen;
    ghal_surface_t *old_s;
    ghal_surface_t *new_s;
    uint16_t new_gen;
    if (!desc || !out_new) return KZ_EINVAL;
    if (kz_surface_validate(desc) != KZ_OK) return KZ_EINVAL;
    if (split_handle(h, &index, &gen) != 0) return KZ_EINVAL;
    spinlock_lock(&g_adapter_lock);
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTALE;
    }
    if (g_slots[index].state == KZ_SURF_IN_USE) {
        uint64_t fence = g_slots[index].fence;
        spinlock_unlock(&g_adapter_lock);
        if (ghal_fence_pending(fence)) return KZ_EBUSY;
        spinlock_lock(&g_adapter_lock);
        if (!g_slots[index].used || g_slots[index].generation != gen) {
            spinlock_unlock(&g_adapter_lock);
            return KZ_ESTALE;
        }
    }
    old_s = g_slots[index].surface;
    spinlock_unlock(&g_adapter_lock);

    new_s = ghal_surface_create(desc->width, desc->height,
                                map_format(desc->format));
    if (!new_s) return KZ_ENOMEM;

    spinlock_lock(&g_adapter_lock);
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        ghal_surface_destroy(new_s);
        return KZ_ESTALE;
    }
    new_gen = g_next_gen;
    if (new_gen == 0) new_gen = 1;
    g_next_gen = new_gen + 1;
    if (g_next_gen == 0) g_next_gen = 1;
    g_slots[index].generation = new_gen;
    g_slots[index].desc = *desc;
    g_slots[index].state = KZ_SURF_CREATED;
    g_slots[index].surface = new_s;
    g_slots[index].fence = 0;
    *out_new = make_handle(index, new_gen);
    spinlock_unlock(&g_adapter_lock);
    if (old_s) ghal_surface_destroy(old_s);
    return KZ_OK;
}

kz_status_t kz_adapter_get(kz_handle_t h, kz_surface_desc_t *out_desc,
                            uint32_t *out_state) {
    uint32_t index;
    uint16_t gen;
    if (split_handle(h, &index, &gen) != 0) return KZ_EINVAL;
    spinlock_lock(&g_adapter_lock);
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTALE;
    }
    if (out_desc) *out_desc = g_slots[index].desc;
    if (out_state) *out_state = (uint32_t)g_slots[index].state;
    spinlock_unlock(&g_adapter_lock);
    return KZ_OK;
}

kz_status_t kz_adapter_upload_shm(int32_t task, kz_handle_t ah,
                                   uint64_t shm_h, const kz_rect_t *rect) {
    uint32_t index;
    uint16_t gen;
    ghal_surface_t *s;
    uint32_t adapter_fmt, adapter_w, adapter_h;
    uint64_t *pages;
    uint32_t npages, stride, sw, sh, sfmt;
    int32_t rc;
    uint64_t xe, ye;
    int64_t r, rend;
    kz_status_t st = KZ_OK;

    if (!rect) return KZ_EINVAL;
    if (rect->w <= 0 || rect->h <= 0 || rect->x < 0 || rect->y < 0)
        return KZ_EINVAL;
    if (split_handle(ah, &index, &gen) != 0) return KZ_EINVAL;

    spinlock_lock(&g_adapter_lock);
    if (!g_slots[index].used || g_slots[index].generation != gen) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTALE;
    }
    if (g_slots[index].state != KZ_SURF_WRITABLE) {
        spinlock_unlock(&g_adapter_lock);
        return KZ_ESTATE;
    }
    s = g_slots[index].surface;
    adapter_fmt = g_slots[index].desc.format;
    adapter_w = g_slots[index].desc.width;
    adapter_h = g_slots[index].desc.height;
    spinlock_unlock(&g_adapter_lock);

    rc = kz_shm_consume(task, shm_h, &pages, &npages, &stride, &sw, &sh,
                        &sfmt);
    if (rc != KZ_OK) return (kz_status_t)rc;
    xe = (uint64_t)rect->x + (uint64_t)rect->w;
    ye = (uint64_t)rect->y + (uint64_t)rect->h;
    if (xe > sw || ye > sh || xe > adapter_w || ye > adapter_h ||
        sfmt != adapter_fmt) {
        kz_shm_release(task, shm_h);
        if (sfmt != adapter_fmt) return KZ_ENOSUP;
        return KZ_EINVAL;
    }

    rend = (int64_t)rect->y + rect->h;
    // ghal upload rects are dual-purpose: source offset AND destination
    // position (sw_surface_upload writes to the same coords). Every chunk
    // therefore carries its surface position, with the src base rewound
    // so src + (dy*stride + dx) lands on the chunk start. Only the chunk
    // bytes are ever dereferenced.
    for (r = rect->y; r < rend;) {
        uint64_t b0 = ((uint64_t)r * stride + (uint64_t)rect->x) * 4u;
        uint64_t b1 = b0 + (uint64_t)rect->w * 4u - 1u;
        uint64_t pg = b0 / 4096u;
        if (b1 / 4096u != pg) {
            // Wide row spans pages: one chunk per page.
            uint64_t c = b0;
            while (c <= b1) {
                uint64_t cp = c / 4096u;
                uint64_t ce =
                    (cp == b1 / 4096u) ? b1 : (cp + 1u) * 4096u - 1u;
                uint32_t k = (uint32_t)((ce - c + 1u) / 4u);
                int32_t dx = rect->x + (int32_t)((c - b0) / 4u);
                ghal_rect_t gr;
                const uint32_t *src;
                uint64_t base;
                if (cp >= npages) {
                    st = KZ_ERANGE;
                    break;
                }
                base = (uint64_t)pages[cp] + hhdm_offset + (c - cp * 4096u);
                src = (const uint32_t *)(uint64_t)(base -
                                                   ((uint64_t)r * stride +
                                                    (uint64_t)dx) *
                                                       4u);
                gr.x = (uint32_t)dx;
                gr.y = (uint32_t)r;
                gr.w = k;
                gr.h = 1;
                ghal_surface_upload(s, src, stride, gr);
                c = ce + 1u;
            }
            if (st != KZ_OK) break;
            r++;
            continue;
        }
        {
            int64_t n = 1;
            ghal_rect_t gr;
            const uint32_t *src;
            uint64_t base;
            while (r + n < rend) {
                uint64_t nb0 =
                    ((uint64_t)(r + n) * stride + (uint64_t)rect->x) * 4u;
                uint64_t nb1 = nb0 + (uint64_t)rect->w * 4u - 1u;
                if (nb0 / 4096u != pg || nb1 / 4096u != pg) break;
                n++;
            }
            if (pg >= npages) {
                st = KZ_ERANGE;
                break;
            }
            base = (uint64_t)pages[pg] + hhdm_offset + (b0 - pg * 4096u);
            src = (const uint32_t *)(uint64_t)(base -
                                               ((uint64_t)r * stride +
                                                (uint64_t)rect->x) *
                                                   4u);
            gr.x = (uint32_t)rect->x;
            gr.y = (uint32_t)r;
            gr.w = (uint32_t)rect->w;
            gr.h = (uint32_t)n;
            ghal_surface_upload(s, src, stride, gr);
            r += n;
        }
    }

    kz_shm_release(task, shm_h);

    spinlock_lock(&g_adapter_lock);
    if (g_slots[index].used && g_slots[index].generation == gen &&
        g_slots[index].state == KZ_SURF_WRITABLE && st == KZ_OK)
        g_slots[index].state = KZ_SURF_READY;
    spinlock_unlock(&g_adapter_lock);
    return st;
}
