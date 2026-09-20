#ifndef INTEL_GEN12_TEST_UTIL_H
#define INTEL_GEN12_TEST_UTIL_H

#include "intel_gpu_alloc.h"
#include "intel_gen12_ppgtt.h"
#include "intel_gen12_batch.h"
#include "intel_gen12_fence.h"
#include "intel_gen12_submit.h"
#include <stdint.h>

// ============================================================
// Gen12 HW-test pixel helpers — AL-10
// Per-page CPU access (pool pages may be non-contiguous).
// Test-only; header-static so Gen12 test modules share without
// a new translation unit.
// ============================================================

static inline void gen12t_write(intel_gpu_buffer_t* b, uint32_t x, uint32_t y,
                                uint32_t v) {
    extern uint64_t hhdm_offset;
    uint64_t byte = ((uint64_t)y * b->stride + x) * 4;
    uint32_t page = (uint32_t)(byte >> 12);
    uint32_t off = (uint32_t)(byte & 0xFFF);
    if (page >= b->num_pages) return;
    *(uint32_t*)(b->phys_addrs[page] + hhdm_offset + off) = v;
}

static inline uint32_t gen12t_read(intel_gpu_buffer_t* b, uint32_t x,
                                   uint32_t y) {
    extern uint64_t hhdm_offset;
    uint64_t byte = ((uint64_t)y * b->stride + x) * 4;
    uint32_t page = (uint32_t)(byte >> 12);
    uint32_t off = (uint32_t)(byte & 0xFFF);
    if (page >= b->num_pages) return 0xDEADDEADu;
    return *(uint32_t*)(b->phys_addrs[page] + hhdm_offset + off);
}

static inline void gen12t_flush(intel_gpu_buffer_t* b) {
    for (uint32_t p = 0; p < b->num_pages; p++)
        gen12_cpu_clflush(b->phys_addrs[p], 4096);
}

// Stage a sealed batch into the ring at the current tail and run it.
// 0 on GPU completion, -1 otherwise (prints reason; never fatal).
static inline int gen12t_run_batch(const char* name, gen12_batch_t* b,
                                   gen12_fence_t* f) {
    extern void serial_print(const char* s);
    // NOTE: commit BEFORE free — free() clears b->cpu.
    int submitted = gen12_submit_commit(b->cpu, b->n);
    if (submitted != 0) {
        serial_print(name); serial_print(": BLOCKED (submit)\n");
        return -1;
    }
    if (gen12_fence_wait(f, 0) != 0) {
        serial_print(name); serial_print(": BLOCKED (no completion)\n");
        return -1;
    }
    return 0;
}

#endif // INTEL_GEN12_TEST_UTIL_H
