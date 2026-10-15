#ifndef KZ_ADAPTER_H
#define KZ_ADAPTER_H

#include <stdint.h>
#include "kz_surface.h"
#include "kz_sync.h"
#include "kz_render.h"

// ============================================================
// R2 Surface-GHAL Adapter (include/kz_adapter.h)
//
// Binds a validated kz_surface_desc to a backend ghal_surface_t and
// enforces the R1 lifecycle at the boundary. Task context only (uses
// plain spinlock, no IRQ masking; never call from IRQ or panic path).
//
// --- Ownership ---
// CPU pixels are ALWAYS caller-owned: the adapter borrows them for upload
// and never frees them. Backend ghal_surface_t is adapter-owned: created
// on kz_adapter_create, destroyed on destroy/resize-retire. No handle
// outlives its resource: destroy bumps generation, stale use -> KZ_ESTALE,
// double destroy -> KZ_ESTALE (never use-after-free).
//
// --- Fences / quarantine ---
// present_checked()==0 moves READY->IN_USE and records the fence.
// IN_USE->RELEASED only via kz_adapter_poll observing !pending.
// Destroying an in-flight resource quarantines the backend surface (fixed
// 8-slot array, no allocation) and invalidates the handle immediately;
// the surface is freed when its fence retires. A timeout NEVER releases:
// kz_adapter_poll returns KZ_EBUSY while pending, quarantine stays.
// Quarantine full -> KZ_EBUSY (poll, then retry).
//
// --- Errors ---
// Backend alloc failure -> KZ_ENOMEM. Submit rejected -> KZ_EBUSY (damage
// retained by caller, retry later). Unsupported format -> KZ_EINVAL (only
// XRGB/ARGB exist; no silent fallback).
// ============================================================

typedef uint32_t kz_handle_t;  // 0 = invalid; never 0 for a live resource

#define KZ_ADAPTER_MAX_RES 32
#define KZ_ADAPTER_MAX_QUARANTINE 8

kz_status_t kz_adapter_create(const kz_surface_desc_t *desc,
                               kz_handle_t *out);
kz_status_t kz_adapter_acquire(kz_handle_t h);
kz_status_t kz_adapter_upload(kz_handle_t h, const uint32_t *pixels,
                               uint32_t src_pitch_px, const kz_rect_t *rect);
kz_status_t kz_adapter_present(kz_handle_t h, const kz_rect_t *rect);
kz_status_t kz_adapter_poll(kz_handle_t h);
kz_status_t kz_adapter_destroy(kz_handle_t h);
kz_status_t kz_adapter_resize(kz_handle_t h, const kz_surface_desc_t *desc,
                               kz_handle_t *out_new);
kz_status_t kz_adapter_get(kz_handle_t h, kz_surface_desc_t *out_desc,
                            uint32_t *out_state);

// R4: upload validated SHM content into an acquired (WRITABLE) adapter
// surface, then mark it READY. Consumes a transient SHM consumer ref
// internally (released before return); SHM pages are never mapped here,
// only read via HHDM in page-sized chunks. Present stays a separate,
// explicit step. Rect must lie inside the SHM dims; formats must match
// (no conversion, never faked). Validation order: adapter handle/state
// first (local fail-fast), then the remote SHM handle.
kz_status_t kz_adapter_upload_shm(int32_t task, kz_handle_t adapter,
                                   uint64_t shm_handle,
                                   const kz_rect_t *rect);

#endif
