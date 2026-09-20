#ifndef INTEL_GEN12_H
#define INTEL_GEN12_H

#include <stdint.h>

// ============================================================
// Gen12 (Alder Lake) engine discovery — AL-3
// (graphics/backend/intel/intel_gen12.h)
//
// Record-only: which copy engine exists, by platform table (same
// method i915 uses — static device info, no MMIO probing).
// No engine is enabled here; submission arrives in AL-5/AL-8.
// ============================================================

typedef enum {
    GEN12_ENG_UNKNOWN = 0,
    GEN12_ENG_BCS0    = 1,   // blitter/copy engine 0
} gen12_copy_engine_t;

typedef struct {
    int copy_present;        // 1 = copy engine identified
    gen12_copy_engine_t copy;
    int blocked;             // 1 = Gen12 but platform topology unknown
} gen12_engines_t;

extern gen12_engines_t g_gen12_engines;

// 0 = copy engine recorded; -1 = SKIP (not Gen12) or BLOCKED
// (Gen12, unknown platform). Never fatal — caller falls back.
int intel_gen12_engine_discovery(void);

// Prints the AL-3 diagnostic; silent unless Gen12 was seen.
void intel_gen12_engine_diag(void);

// --- AL-4: Gen12 GPU virtual memory (GGTT-only minimum) ---
// 0 = gpu_addr/len inside the GGTT window, 4K-aligned, non-empty.
int intel_gen12_vm_valid(uint64_t gpu_addr, uint64_t len);
// Boot self-test: pure validator checks always; live overlap checks
// when GTT is programmed. Prints PASS / SKIP / FAIL, never fatal.
void intel_gen12_vm_selftest(void);

#endif // INTEL_GEN12_H
