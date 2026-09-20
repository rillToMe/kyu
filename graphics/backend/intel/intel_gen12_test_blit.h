#ifndef INTEL_GEN12_TEST_BLIT_H
#define INTEL_GEN12_TEST_BLIT_H

// ============================================================
// Gen12 hardware BLIT tests — AL-11
// full / subrect / offset-dst / multi-in-one-batch / chained
// cross-buffer (A→B→C ordering in one submit).
// Contract stays 1:1, no scaling; same-buffer overlap is rejected
// by the Phase-12 gate (no undefined GPU overlap semantics).
// ============================================================

void gen12_test_blit_run(void);

#endif // INTEL_GEN12_TEST_BLIT_H
