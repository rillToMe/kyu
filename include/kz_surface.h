#ifndef KZ_SURFACE_H
#define KZ_SURFACE_H

#include <stdint.h>
#include <stddef.h>

// ============================================================
// R1 Surface Contract (include/kz_surface.h)
//
// Backend-independent surface descriptor + lifecycle. This header is
// standalone C (stdint/stddef only) so kernel, host tests, and userspace
// share one contract without include tangles.
//
// It does NOT replace DisplayBuffer (kernel/display.h) or the opaque
// ghal_surface_t (graphics/ghal.h). Those stay as-is; this contract
// describes the metadata every surface-like object must satisfy and gives
// overflow-safe validators + an explicit lifecycle both sides map onto.
//
// --- Format / alpha semantics ---
//   KZ_FMT_XRGB8888 = 0x00RRGGBB in memory. No alpha channel; the top byte
//     of a pixel is IGNORED on read and written as 0x00. Paint alpha still
//     applies at draw time (src-over onto an opaque destination).
//     Value-mirrors DISPLAY_FMT_XRGB8888 and GHAL_FMT_XRGB8888.
//   KZ_FMT_ARGB8888 = 0xAARRGGBB in memory. Straight (non-premultiplied)
//     alpha per pixel; compositing is src-over. Mirrors GHAL_FMT_ARGB8888.
//     Used for cursor images and offscreen layers, never for scanout.
//
// --- Coordinates ---
// All rects are integer half-open [x, x+w) x [y, y+h), matching Rect in
// display.h. Logical coordinates == physical pixels in R1 (no device scale;
// stride stays in pixels, never scaled).
// ============================================================

typedef enum {
    KZ_FMT_XRGB8888 = 0,
    KZ_FMT_ARGB8888 = 1,
} kz_format_t;

typedef enum {
    KZ_ALPHA_IGNORED  = 0,  // XRGB: per-pixel top byte meaningless
    KZ_ALPHA_STRAIGHT = 1,  // ARGB: straight alpha, src-over blend
} kz_alpha_t;

// Usage flags: what may touch the pixels. A surface usable as scanout must
// set KZ_USAGE_SCANOUT; a surface the CPU writes must set KZ_USAGE_CPU_WRITE.
#define KZ_USAGE_CPU_READ   (1u << 0)
#define KZ_USAGE_CPU_WRITE  (1u << 1)
#define KZ_USAGE_SCANOUT    (1u << 2)
#define KZ_USAGE_GPU_READ   (1u << 3)
#define KZ_USAGE_GPU_WRITE  (1u << 4)

// Bounds shared by every implementation (DISPLAY_MAX_DIM / GHAL_MAX_DIM).
#define KZ_SURFACE_MAX_DIM 8192u
// Largest single surface: 8192*8192*4 = 256 MiB; creation without a caller
// limit above this is rejected, and kernel callers keep their tighter
// 16 MiB (KWM) / mode-sized caps on top.
#define KZ_SURFACE_MAX_BYTES (256u * 1024u * 1024u)

typedef struct {
    uint32_t width;        // pixels, > 0
    uint32_t height;       // pixels, > 0
    uint32_t stride_px;    // pixels per row, >= width
    uint32_t format;       // kz_format_t
    uint64_t size_bytes;   // backing store size; must cover stride*h*4
    uint32_t usage;        // KZ_USAGE_* bitmask, nonzero
    uint64_t id;           // resource identity (backend-assigned)
    uint32_t generation;   // bumped on destroy/recreate; rejects stale refs
    uint32_t owner;        // owner tag (task id / backend id)
} kz_surface_desc_t;

// --- Lifecycle ---
// UNINIT -> CREATED -> WRITABLE -> READY -> IN_USE -> RELEASED -+
//              ^                                               |
//              +------------------ (reacquire for next frame) -+
// Any state -> DESTROYED (terminal; id+generation must not be reused
// without bumping generation). Software/sync backends may fold
// READY/IN_USE/RELEASED into one immediate step; async backends must walk
// every step and gate IN_USE->RELEASED on fence completion (kz_sync.h).
typedef enum {
    KZ_SURF_UNINIT = 0,
    KZ_SURF_CREATED,
    KZ_SURF_WRITABLE,
    KZ_SURF_READY,
    KZ_SURF_IN_USE,
    KZ_SURF_RELEASED,
    KZ_SURF_DESTROYED,
} kz_surface_state_t;

typedef enum {
    KZ_OK       = 0,
    KZ_EINVAL   = -1,  // null arg, bad dims/format/stride, unknown enum
    KZ_ENOMEM   = -2,  // allocation failed (caller decides limit)
    KZ_ENOSUP   = -3,  // valid request the backend cannot do (never fake OK)
    KZ_EBUSY    = -4,  // surface in use by a consumer; retry after release
    KZ_ERANGE   = -5,  // arithmetic overflow or out-of-bounds access
    KZ_ESTALE   = -6,  // id/generation no longer live
    KZ_ESTATE   = -7,  // illegal lifecycle transition
} kz_status_t;

// Overflow-safe: bytes needed for w*h 32bpp pixels. 0 on overflow.
static inline uint64_t kz_surface_bytes(uint32_t w, uint32_t h) {
    uint64_t px = (uint64_t)w * h;
    if (w != 0 && px / w != h) return 0;
    uint64_t bytes = px * 4u;
    if (px != 0 && bytes / px != 4u) return 0;
    return bytes;
}

// Validate a descriptor without touching pixels. Returns KZ_*.
static inline kz_status_t kz_surface_validate(const kz_surface_desc_t *d) {
    if (!d) return KZ_EINVAL;
    if (d->width == 0 || d->height == 0) return KZ_EINVAL;
    if (d->width > KZ_SURFACE_MAX_DIM || d->height > KZ_SURFACE_MAX_DIM)
        return KZ_EINVAL;
    if (d->format != KZ_FMT_XRGB8888 && d->format != KZ_FMT_ARGB8888)
        return KZ_EINVAL;
    if (d->usage == 0) return KZ_EINVAL;
    if (d->stride_px < d->width) return KZ_EINVAL;
    if (d->stride_px > KZ_SURFACE_MAX_DIM * 2u) return KZ_EINVAL;
    uint64_t need = (uint64_t)d->stride_px * d->height * 4u;
    if (d->height != 0 && need / d->height / 4u != d->stride_px) return KZ_ERANGE;
    if (need > KZ_SURFACE_MAX_BYTES) return KZ_EINVAL;
    if (d->size_bytes < need) return KZ_EINVAL;
    return KZ_OK;
}

// Legal-transition check for the lifecycle above. Pure; no side effects.
static inline kz_status_t kz_surface_transition(kz_surface_state_t from,
                                                kz_surface_state_t to) {
    switch (from) {
    case KZ_SURF_UNINIT:
        return to == KZ_SURF_CREATED ? KZ_OK : KZ_ESTATE;
    case KZ_SURF_CREATED:
        return (to == KZ_SURF_WRITABLE || to == KZ_SURF_DESTROYED) ? KZ_OK : KZ_ESTATE;
    case KZ_SURF_WRITABLE:
        return (to == KZ_SURF_READY || to == KZ_SURF_DESTROYED) ? KZ_OK : KZ_ESTATE;
    case KZ_SURF_READY:
        return (to == KZ_SURF_IN_USE || to == KZ_SURF_DESTROYED) ? KZ_OK : KZ_ESTATE;
    case KZ_SURF_IN_USE:
        return (to == KZ_SURF_RELEASED || to == KZ_SURF_DESTROYED) ? KZ_OK : KZ_ESTATE;
    case KZ_SURF_RELEASED:
        return (to == KZ_SURF_WRITABLE || to == KZ_SURF_DESTROYED) ? KZ_OK : KZ_ESTATE;
    case KZ_SURF_DESTROYED:
    default:
        return KZ_ESTATE;
    }
}

// Generation check: a reference (id, generation) is live only when both
// match the current record.
static inline kz_status_t kz_surface_check_gen(uint64_t live_id, uint32_t live_gen,
                                               uint64_t ref_id, uint32_t ref_gen) {
    if (ref_id != live_id || ref_gen != live_gen) return KZ_ESTALE;
    return KZ_OK;
}

#endif
