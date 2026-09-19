#ifndef INTEL_GEN12_ROBUST_H
#define INTEL_GEN12_ROBUST_H

// ============================================================
// Gen12 robustness suite — AL-16
// Extends the Phase-22 suite: every Gen12 constructor, emitter,
// validator and submit entry Sang-so: NULL, empty, oversized,
// misaligned, OOB, destroyed, unmapped, invalid-fence and
// not-live cases. Expected: reject (-1) → bounded → CPU
// fallback. A GPU error must never become a kernel panic, and
// this suite must never touch HW on a dead engine.
// ============================================================

void gen12_robust_selftest(void);

#endif // INTEL_GEN12_ROBUST_H
