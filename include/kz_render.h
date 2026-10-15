#ifndef KZ_RENDER_H
#define KZ_RENDER_H

#include <stdint.h>
#include <stddef.h>
#include "kz_surface.h"
#include "kz_sync.h"

// ============================================================
// R1 Native 2D Rendering API (include/kz_render.h)
//
// Stable, backend-independent C API. R1 ships the software reference
// (libs/render/kz_render.c); GPU execution reuses a backend op only where
// the backend genuinely implements it (capability-checked, never faked).
//
// --- Ownership ---
// kz_context_create BORROWS the target: it never retains, frees, or keeps
// the pixels past kz_context_destroy. The caller keeps the surface (and its
// pixel store) alive for the context's whole lifetime.
//
// --- Coordinates ---
// Integer pixels, half-open rects [x,x+w)x[y,y+h), origin top-left.
// Transform is scale+translate only: {sx, sy, tx, ty} mapping
//   dx = x*sx/1024 + tx,  dy = y*sy/1024 + ty   (1024 = KZ_FIXED_ONE).
// Anything else (rotate/skew) returns KZ_ENOSUP. R1 honours the transform
// on rect ops; general matrices and paths are R2.
//
// --- State ---
// save/restore pushes {transform, clip} as one entry, depth KZ_STATE_MAX (8).
// clip_rect intersects with the current clip; an empty intersection clips
// everything until restored (never an error).
//
// --- Paint ---
// Solid color 0x00RRGGBB + opacity 0..255. Alpha blends src-over onto the
// target (XRGB dst treated as opaque); opacity 255 overwrites. Alpha phase
// matches color_blend_alpha semantics.
//
// --- Errors ---
// Null context/out/paint/rect -> KZ_EINVAL. Empty rect (w/h 0) -> KZ_OK no-op
// (matches sw_fill_rect). Out-of-target writes are clipped, never errors.
// Unsupported op -> KZ_ENOSUP (never fake success).
//
// --- Flush ---
// kz_flush submits all queued work. Software completes synchronously:
// *out_fence = NULL (nothing in flight) and every stage is RELEASED.
// Async backends return a live fence proving SUBMITTED only.
//
// --- Threading ---
// A context is single-owner, non-reentrant. One thread at a time.
// ============================================================

#define KZ_FIXED_ONE 1024
#define KZ_STATE_MAX 8
#define KZ_CLIP_MAX  8

typedef struct {
    int32_t x, y;
    int32_t w, h;   // >= 0; 0 = empty (no-op, KZ_OK)
} kz_rect_t;

typedef struct {
    int32_t sx, sy;  // 1024 = 1.0
    int32_t tx, ty;  // pixels
} kz_transform_t;

typedef struct {
    uint32_t color;  // 0x00RRGGBB
    uint32_t alpha;  // 0..255 (clamped); 255 = opaque overwrite
} kz_paint_t;

// Kyuzen has no global malloc: kernel = kmalloc, userspace = sys_alloc,
// host test = malloc. The caller injects both (mirrors kz_heap_t); NULL
// allocator -> KZ_EINVAL, allocation failure -> KZ_ENOMEM.
typedef void *(*kz_alloc_fn)(uint32_t size);
typedef void (*kz_free_fn)(void *ptr);

typedef struct kz_context kz_context_t;

// Target wraps caller pixels (borrowed) or allocates owned storage.
// pixels: row-major 32bpp, pitch in pixels (>= width). Must stay alive
// while any context targets it. Returns KZ_*; *out set only on KZ_OK.
kz_status_t kz_target_wrap(uint32_t *pixels, uint32_t width, uint32_t height,
                            uint32_t stride_px, uint32_t format,
                            kz_surface_desc_t *out_desc);
kz_status_t kz_target_create(uint32_t width, uint32_t height, uint32_t format,
                              kz_alloc_fn alloc, kz_free_fn free_fn,
                              kz_surface_desc_t *out_desc,
                              uint32_t **out_pixels);
void kz_target_free(const kz_surface_desc_t *desc, uint32_t *pixels,
                    kz_free_fn free_fn);

kz_status_t kz_context_create(const kz_surface_desc_t *target,
                               uint32_t *pixels,
                               kz_alloc_fn alloc, kz_free_fn free_fn,
                               kz_context_t **out_ctx);
void kz_context_destroy(kz_context_t *ctx, kz_free_fn free_fn);

kz_status_t kz_save(kz_context_t *ctx);
kz_status_t kz_restore(kz_context_t *ctx);
kz_status_t kz_set_transform(kz_context_t *ctx, const kz_transform_t *t);
kz_status_t kz_clip_rect(kz_context_t *ctx, const kz_rect_t *r);

kz_status_t kz_fill_rect(kz_context_t *ctx, const kz_rect_t *r,
                          const kz_paint_t *paint);
kz_status_t kz_draw_rect(kz_context_t *ctx, const kz_rect_t *r,
                          const kz_paint_t *paint);
kz_status_t kz_clear(kz_context_t *ctx, uint32_t color);

// Blit: copies src_rect -> dst_rect. Sizes must match (no scaling in R1),
// formats must match. Overlap within one target is safe (memmove order).
// Paint alpha combines with per-pixel alpha for ARGB sources:
// a_eff = (src_a * opacity + 127) / 255, then src-over.
kz_status_t kz_blit(kz_context_t *ctx,
                     const uint32_t *src_pixels,
                     const kz_surface_desc_t *src,
                     const kz_rect_t *src_rect,
                     const kz_rect_t *dst_rect,
                     const kz_paint_t *paint);

// Submit queued work. Software: synchronous, *out_fence = NULL.
kz_status_t kz_flush(kz_context_t *ctx, kz_fence_t **out_fence);

#endif
