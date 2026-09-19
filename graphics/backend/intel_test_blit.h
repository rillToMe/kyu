#ifndef INTEL_TEST_BLIT_H
#define INTEL_TEST_BLIT_H

// ============================================================
// Intel GPU BLIT test — Phase 11
//
// 1:1 surface-to-surface blits (no scaling): full-buffer,
// sub-rect with src/dst offset, and multi-blit submission.
// Validated per-pixel on CPU. 0 = PASS, 1 = SKIP, -1 = FAIL.
// Never fatal.
// ============================================================

int intel_test_blit_run(void);

#endif // INTEL_TEST_BLIT_H
