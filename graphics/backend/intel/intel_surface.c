// ============================================================
// Intel GPU-backed surface — Phase 16
// (graphics/backend/intel/intel_surface.c)
// ============================================================

#include "intel_surface.h"
#include "intel_gpu_alloc.h"

extern uint64_t hhdm_offset;
extern void serial_print(const char* s);

int intel_gsurf_create(intel_gsurf_t* s, uint32_t w, uint32_t h) {
    if (!s) return -1;
    s->buf_id = -1; s->w = s->h = s->stride = 0;
    if (w == 0 || h == 0) return -1;
    int id = intel_gpu_buffer_create(w, h);
    if (id < 0) return -1;
    if (intel_gpu_buffer_map_gpu(id) != 0) {
        intel_gpu_buffer_destroy(id);
        return -1;
    }
    intel_gpu_buffer_t* b = intel_gpu_buffer_get(id);
    if (!b) { intel_gpu_buffer_destroy(id); return -1; }
    s->buf_id = id;
    s->w = b->width; s->h = b->height; s->stride = b->stride;
    return 0;
}

void intel_gsurf_destroy(intel_gsurf_t* s) {
    if (!s || s->buf_id < 0) return;
    intel_gpu_buffer_destroy(s->buf_id);
    s->buf_id = -1; s->w = s->h = s->stride = 0;
}

int intel_gsurf_valid(const intel_gsurf_t* s) {
    if (!s || s->buf_id < 0 || s->w == 0 || s->h == 0) return 0;
    return intel_gpu_buffer_get(s->buf_id) ? 1 : 0;
}

uint64_t intel_gsurf_gpu_addr(const intel_gsurf_t* s) {
    if (!intel_gsurf_valid(s)) return 0;
    return intel_gpu_buffer_gpu_vaddr(s->buf_id);
}

uint32_t intel_gsurf_stride(const intel_gsurf_t* s) {
    if (!s) return 0;
    return s->stride;
}

static intel_gpu_buffer_t* buf_of(intel_gsurf_t* s) {
    if (!intel_gsurf_valid(s)) return 0;
    return intel_gpu_buffer_get(s->buf_id);
}

void intel_gsurf_write(intel_gsurf_t* s, uint32_t x, uint32_t y, uint32_t v) {
    intel_gpu_buffer_t* b = buf_of(s);
    if (!b || x >= s->w || y >= s->h) return;
    uint64_t byte = ((uint64_t)y * b->stride + x) * 4;
    uint32_t page = (uint32_t)(byte >> 12);
    if (page >= b->num_pages) return;
    *(uint32_t*)(b->phys_addrs[page] + hhdm_offset + (byte & 0xFFF)) = v;
}

uint32_t intel_gsurf_read(intel_gsurf_t* s, uint32_t x, uint32_t y) {
    intel_gpu_buffer_t* b = buf_of(s);
    if (!b || x >= s->w || y >= s->h) return 0;
    uint64_t byte = ((uint64_t)y * b->stride + x) * 4;
    uint32_t page = (uint32_t)(byte >> 12);
    if (page >= b->num_pages) return 0;
    return *(uint32_t*)(b->phys_addrs[page] + hhdm_offset + (byte & 0xFFF));
}

int intel_gsurf_selftest(void) {
    // Reject zero-size.
    intel_gsurf_t bad;
    if (intel_gsurf_create(&bad, 0, 64) == 0) {
        serial_print("[intel_surface] SURFACE: FAIL (zero-accept)\n");
        return -1;
    }
    // Round-trip 64x64.
    intel_gsurf_t s;
    if (intel_gsurf_create(&s, 64, 64) != 0) {
        serial_print("[intel_surface] SURFACE: FAIL (alloc)\n");
        return -1;
    }
    if (!intel_gsurf_valid(&s) || intel_gsurf_gpu_addr(&s) == 0 ||
        intel_gsurf_stride(&s) != 64) {
        intel_gsurf_destroy(&s);
        serial_print("[intel_surface] SURFACE: FAIL (geometry)\n");
        return -1;
    }
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++)
            intel_gsurf_write(&s, x, y, 0xFF000000u | (y * 64 + x));
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++)
            if (intel_gsurf_read(&s, x, y) != (0xFF000000u | (y * 64 + x))) {
                intel_gsurf_destroy(&s);
                serial_print("[intel_surface] SURFACE: FAIL (mismatch)\n");
                return -1;
            }
    // OOB reads return 0, OOB writes ignored.
    if (intel_gsurf_read(&s, 64, 0) != 0) {
        intel_gsurf_destroy(&s);
        serial_print("[intel_surface] SURFACE: FAIL (oob)\n");
        return -1;
    }
    intel_gsurf_destroy(&s);
    if (intel_gsurf_valid(&s)) {
        serial_print("[intel_surface] SURFACE: FAIL (destroy)\n");
        return -1;
    }
    serial_print("[intel_surface] SURFACE: PASS\n");
    return 0;
}
