#ifndef INTEL_GEN12_FAULT_H
#define INTEL_GEN12_FAULT_H

// ============================================================
// Gen12 fault / timeout handling — AL-17
// Proves bounded failure: timeouts return, the system stays
// alive (later ops still work), unusable HW converges to
// acceleration-disabled + software fallback. No GPU reset
// exists anywhere here (none implemented, none faked).
// ============================================================

void gen12_fault_selftest(void);

#endif // INTEL_GEN12_FAULT_H
