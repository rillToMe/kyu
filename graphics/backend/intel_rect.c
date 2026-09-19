// ============================================================
// Intel rect / boundary validation — Phase 12
// (graphics/backend/intel_rect.c)
// ============================================================

#include "intel_rect.h"

extern void serial_print(const char* s);

static int buf_ok(const intel_gpu_buffer_t* b) {
    if (!b) return -1;
    if (b->state != INTEL_BUF_MAPPED || !b->gpu_mapped) return -1;
    if (b->width == 0 || b->height == 0 || b->stride == 0) return -1;
    if (b->gpu_vaddr == 0) return -1;
    return 0;
}

int intel_rect_clip(intel_rect_t* r, uint32_t bw, uint32_t bh) {
    if (!r || bw == 0 || bh == 0) return -1;
    if (r->w == 0 || r->h == 0) return -1;
    if (r->x >= bw || r->y >= bh) return -1; // incl. negative-as-huge-uint
    uint64_t xe = (uint64_t)r->x + r->w;     // 64b: x+w overflow-safe
    uint64_t ye = (uint64_t)r->y + r->h;
    if (xe > bw) r->w = bw - r->x;
    if (ye > bh) r->h = bh - r->y;
    if (r->w == 0 || r->h == 0) return -1;
    return 0;
}

int intel_fill_validate(const intel_gpu_buffer_t* buf, intel_rect_t* r) {
    if (buf_ok(buf) || !r) return -1;
    return intel_rect_clip(r, buf->width, buf->height);
}

static int overlap(intel_rect_t* a, intel_rect_t* b) {
    uint64_t ax2 = (uint64_t)a->x + a->w, ay2 = (uint64_t)a->y + a->h;
    uint64_t bx2 = (uint64_t)b->x + b->w, by2 = (uint64_t)b->y + b->h;
    return (a->x < bx2 && b->x < ax2 && a->y < by2 && b->y < ay2);
}

int intel_blit_validate(const intel_gpu_buffer_t* dst,
                        const intel_gpu_buffer_t* src,
                        intel_rect_t* s, intel_rect_t* d) {
    if (buf_ok(dst) || buf_ok(src) || !s || !d) return -1;
    if (s->w == 0 || s->h == 0 || d->w == 0 || d->h == 0) return -1;
    if (s->w != d->w || s->h != d->h) return -1; // 1:1 contract
    if (s->x >= src->width || s->y >= src->height) return -1;
    if (d->x >= dst->width || d->y >= dst->height) return -1;
    // Clip pair consistently to both buffers.
    uint32_t w = s->w, h = s->h;
    uint64_t v;
    v = (uint64_t)s->x + w; if (v > src->width) w = src->width - s->x;
    v = (uint64_t)d->x + w; if (v > dst->width) w = dst->width - d->x;
    v = (uint64_t)s->y + h; if (v > src->height) h = src->height - s->y;
    v = (uint64_t)d->y + h; if (v > dst->height) h = dst->height - d->y;
    if (w == 0 || h == 0) return -1;
    s->w = d->w = w;
    s->h = d->h = h;
    // Same-buffer overlap: XY direction undefined -> CPU owns it.
    if (dst->gpu_vaddr == src->gpu_vaddr && overlap(d, s)) return -1;
    return 0;
}

// --- Self-test: fake MAPPED buffers, no HW touch ---

static intel_gpu_buffer_t mkbuf(uint32_t w, uint32_t h, uint64_t vaddr) {
    intel_gpu_buffer_t b;
    b.id = 1; b.state = INTEL_BUF_MAPPED;
    b.width = w; b.height = h; b.stride = w; b.bpp = 32;
    b.size_bytes = (uint64_t)w * h * 4;
    b.num_pages = 1; b.phys_addrs[0] = 0x1000;
    b.cpu_ptr = 0; b.cpu_mapped = 0;
    b.gpu_vaddr = vaddr; b.gtt_offset = 0; b.gpu_mapped = 1;
    return b;
}

int intel_rect_selftest(void) {
    int fails = 0;
#define CHECK(cond) do { if (!(cond)) fails++; } while (0)
    {
        intel_rect_t r = {0, 0, 64, 64};
        CHECK(intel_rect_clip(&r, 64, 64) == 0 && r.w == 64 && r.h == 64);
    }
    {   // zero size
        intel_rect_t r = {0, 0, 0, 8};
        CHECK(intel_rect_clip(&r, 64, 64) != 0);
        r = (intel_rect_t){0, 0, 8, 0};
        CHECK(intel_rect_clip(&r, 64, 64) != 0);
    }
    {   // fully OOB origin
        intel_rect_t r = {64, 0, 8, 8};
        CHECK(intel_rect_clip(&r, 64, 64) != 0);
    }
    {   // negative-as-uint32 (-1 = 0xFFFFFFFF)
        intel_rect_t r = {0xFFFFFFFFu, 0, 8, 8};
        CHECK(intel_rect_clip(&r, 64, 64) != 0);
    }
    {   // partial clip: (56,56,16,16) -> 8x8
        intel_rect_t r = {56, 56, 16, 16};
        CHECK(intel_rect_clip(&r, 64, 64) == 0 && r.w == 8 && r.h == 8);
    }
    {   // x+w overflow: x=0xFFFFFFF0, w=0x100 -> xe wraps in 32b
        intel_rect_t r = {0xFFFFFFF0u, 0, 0x100, 8};
        CHECK(intel_rect_clip(&r, 64, 64) != 0); // origin OOB first
    }
    {   // fill gate: null / unmapped / valid
        intel_rect_t r = {0, 0, 8, 8};
        CHECK(intel_fill_validate(0, &r) != 0);
        intel_gpu_buffer_t u = mkbuf(64, 64, 0x100000);
        u.state = INTEL_BUF_ALLOCATED; u.gpu_mapped = 0;
        CHECK(intel_fill_validate(&u, &r) != 0);
        intel_gpu_buffer_t b = mkbuf(64, 64, 0x100000);
        r = (intel_rect_t){60, 60, 16, 16};
        CHECK(intel_fill_validate(&b, &r) == 0 && r.w == 4 && r.h == 4);
    }
    {   // blit gate: mismatch / overlap / valid offset
        intel_gpu_buffer_t a = mkbuf(64, 64, 0x100000);
        intel_gpu_buffer_t b = mkbuf(64, 64, 0x200000);
        intel_rect_t s = {0, 0, 16, 16}, d = {0, 0, 8, 8};
        CHECK(intel_blit_validate(&b, &a, &s, &d) != 0); // size mismatch
        s = (intel_rect_t){8, 8, 16, 16}; d = (intel_rect_t){32, 32, 16, 16};
        CHECK(intel_blit_validate(&b, &a, &s, &d) == 0);
        s = (intel_rect_t){0, 0, 32, 32}; d = (intel_rect_t){16, 16, 32, 32};
        CHECK(intel_blit_validate(&a, &a, &s, &d) != 0); // same-buf overlap
        s = (intel_rect_t){0, 0, 16, 16}; d = (intel_rect_t){16, 0, 16, 16};
        CHECK(intel_blit_validate(&a, &a, &s, &d) == 0); // adjacent ok
        s = (intel_rect_t){0, 0, 16, 16}; d = (intel_rect_t){60, 60, 16, 16};
        CHECK(intel_blit_validate(&b, &a, &s, &d) == 0 && s.w == 4 && d.w == 4);
    }
#undef CHECK
    if (fails) serial_print("[intel_rect] selftest FAIL\n");
    else serial_print("[intel_rect] selftest PASS\n");
    return fails ? -1 : 0;
}
