#ifndef INTEL_SURFACE_H
#define INTEL_SURFACE_H

// ============================================================
// Intel GPU-backed surface — Phase 16
//
// Surface = GPU buffer (GTT-mapped) + geometry. Independent
// from the physical display framebuffer; format fixed to the
// existing Kyuzen XRGB8888 framebuffer format (no new pixel
// formats). CPU pixel access is per-page (PMM may be
// non-contiguous).
//
// NOT wired into the GHAL vtable yet (that is Phase 17) —
// the vtable keeps its kmalloc CPU path so fallback stays
// intact while this abstraction is proven by self-test.
// ============================================================

#include <stdint.h>

typedef struct {
    int      buf_id;   // intel_gpu_buffer id, -1 = empty
    uint32_t w, h;     // pixels
    uint32_t stride;   // pixels per row
} intel_gsurf_t;

// Alloc + GTT-map. 0 ok, -1 reject (zero/oversize/OOM).
int  intel_gsurf_create(intel_gsurf_t* s, uint32_t w, uint32_t h);
void intel_gsurf_destroy(intel_gsurf_t* s);
int  intel_gsurf_valid(const intel_gsurf_t* s);

// GPU address / stride for command builders. 0 when invalid.
uint64_t intel_gsurf_gpu_addr(const intel_gsurf_t* s);
uint32_t intel_gsurf_stride(const intel_gsurf_t* s);

// CPU pixel access (per-page, bounds-checked, OOB ignored/0).
void     intel_gsurf_write(intel_gsurf_t* s, uint32_t x, uint32_t y, uint32_t v);
uint32_t intel_gsurf_read(intel_gsurf_t* s, uint32_t x, uint32_t y);

// Create/write/read/destroy round-trip (needs GTT, no BCS).
// 0 = PASS, -1 = FAIL. Never fatal.
int intel_gsurf_selftest(void);

#endif // INTEL_SURFACE_H
