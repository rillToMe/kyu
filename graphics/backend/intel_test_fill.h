#ifndef INTEL_TEST_FILL_H
#define INTEL_TEST_FILL_H

// ============================================================
// Intel GPU FILL test — Phase 10
//
// Standalone HW tests (no KWM): full-buffer, small, edge
// (CPU-clipped), unaligned, and multi-rect fills on a 64x64
// buffer, validated on CPU per-pixel.
//
// Returns: 0 = PASS, 1 = SKIP (no BCS), -1 = FAIL.
// Never fatal: boot continues regardless.
// ============================================================

int intel_test_fill_run(void);

#endif // INTEL_TEST_FILL_H
