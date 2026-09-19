#ifndef INTEL_RECT_H
#define INTEL_RECT_H

// ============================================================
// Intel rect / boundary validation — Phase 12
//
// Single gate for every GPU op. Kernel never trusts caller
// coords: all rects are clipped in place, origins outside the
// buffer are rejected, zero sizes rejected, overlapping
// same-buffer blits rejected (XY direction undefined —
// CPU memmove path owns those), unmapped/null buffers rejected.
//
// Overflow-safe: x+w computed in 64 bit (negative userspace
// coords arrive as huge uint32 and fail the origin check).
//
// Future GHAL fill/blit fast paths (Phase 15+) must call
// intel_fill_validate / intel_blit_validate before emitting.
// ============================================================

#include <stdint.h>
#include "intel_gpu_alloc.h"

typedef struct { uint32_t x, y, w, h; } intel_rect_t;

// Clip *r into [0,bw)x[0,bh). 0 = usable (maybe clipped),
// -1 = fully OOB / zero size. Writes back clipped rect.
int intel_rect_clip(intel_rect_t* r, uint32_t bw, uint32_t bh);

// Fill gate: clips *r to buffer. 0 ok, -1 reject.
int intel_fill_validate(const intel_gpu_buffer_t* buf, intel_rect_t* r);

// Blit gate (1:1): clips pair consistently, rejects size
// mismatch and same-buffer overlap. 0 ok, -1 reject.
int intel_blit_validate(const intel_gpu_buffer_t* dst,
                        const intel_gpu_buffer_t* src,
                        intel_rect_t* s, intel_rect_t* d);

// Pure-CPU case battery (no HW touch). 0 = all PASS.
int intel_rect_selftest(void);

#endif // INTEL_RECT_H
