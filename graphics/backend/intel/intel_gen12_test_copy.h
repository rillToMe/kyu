#ifndef INTEL_GEN12_TEST_COPY_H
#define INTEL_GEN12_TEST_COPY_H

// ============================================================
// Gen12 hardware COPY tests — AL-9
// (graphics/backend/intel/intel_gen12_test_copy.h)
//
// 64x64 aligned / unaligned / subregion / page-boundary / 128x128
// multi-page. CPU pattern → clflush → PPGTT sync → ring XY_COPY →
// ELSP → fence → clflush → CPU validate. PASS only on real GPU
// execution; anything else is FAIL / SKIP / BLOCKED, never fatal.
// ============================================================

void gen12_test_copy_run(void);

#endif // INTEL_GEN12_TEST_COPY_H
