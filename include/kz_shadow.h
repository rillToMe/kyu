#ifndef KZ_SHADOW_H
#define KZ_SHADOW_H

#include <stdint.h>
#include "kz_render.h"

// ============================================================
// R2 Shadow Compositor (include/kz_shadow.h)
//
// In-process userspace prototype. Consumes a deterministic scene snapshot
// and renders to its own off-screen target via kz_render. It NEVER touches
// scanout, NEVER presents, and owns no window policy (focus/input/drag/
// lifecycle stay in KWM). The kernel compositor remains the sole active
// display owner throughout R2.
//
// --- Isolation note ---
// KyuzenOS has no cross-process shared-surface mechanism in R2 (no shm/IPC
// for pixels), so a multi-process compositor cannot be built safely yet.
// This prototype runs in-process (same address space as the scene owner)
// and proves the composition semantics + damage handling the future
// out-of-process compositor needs. Process isolation is R3 work.
//
// --- Snapshot model ---
// Layers carry their descriptor BY VALUE: a resized/replaced surface shows
// up as a new snapshot, old references are never dereferenced. Destroyed =
// omitted (or visible=0, skipped safely). Stale detection across processes
// needs the adapter generation check (kz_surface_check_gen) once shared
// surfaces exist.
//
// --- Damage ---
// NULL damage (or n_damage=0) = full redraw. Otherwise each rect is clipped
// to the target; rects may start off-screen (clipped, never an error) but
// negative w/h is rejected. More than KZ_SHADOW_MAX_DAMAGE rects collapses
// to the bounding superset (over-invalidation, never loss). Empty
// intersection = no-op, KZ_OK.
//
// --- Errors ---
// Two-phase: every layer is validated BEFORE any pixel is written, so a
// bad snapshot leaves the target untouched. Compose-phase errors abort
// with the failing layer index in stats (target may be partial — caller
// must discard it, never present it).
//
// The entry transform/clip are saved and restored (identity is used
// internally). The caller clip must cover the target — true for fresh
// contexts; otherwise output is conservatively clipped to it.
// ============================================================

#define KZ_SHADOW_MAX_LAYERS 16
#define KZ_SHADOW_MAX_DAMAGE 64

typedef struct {
    const uint32_t *pixels;   // borrowed, NULL only when visible == 0
    kz_surface_desc_t desc;   // authoritative for this snapshot
    int32_t x, y;             // position on target
    uint32_t z;               // lower = further back
    uint32_t opacity;         // 0..255 (clamped); 0 = invisible but valid
    int visible;              // 0 = skipped safely (destroyed/hidden)
} kz_layer_t;

typedef struct {
    uint32_t damage_rects;    // rects actually rendered
    uint32_t pixels_cleared;  // background pixels written
    uint32_t pixels_blended;  // layer pixels composited
    uint32_t layers_drawn;
    uint32_t layers_skipped;
    uint32_t failed_index;    // KZ_SHADOW_NO_FAIL when no failure
} kz_shadow_stats_t;

#define KZ_SHADOW_NO_FAIL 0xFFFFFFFFu

kz_status_t kz_shadow_compose(kz_context_t *target,
                              const kz_surface_desc_t *target_desc,
                              const kz_layer_t *layers, uint32_t n_layers,
                              const kz_rect_t *damage, uint32_t n_damage,
                              uint32_t background,
                              kz_shadow_stats_t *out_stats);

#endif
