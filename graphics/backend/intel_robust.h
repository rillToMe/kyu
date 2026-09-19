#ifndef INTEL_ROBUST_H
#define INTEL_ROBUST_H

// ============================================================
// Intel failure-path self-test — Phase 22
//
// Exercises every graceful-failure path with invalid input and
// asserts safe rejection (error code, never a crash): bad
// buffer ids/dims, NULL commands, empty submits, unmapped GTT,
// unavailable BCS, NULL fences/rects. Pure CPU, no HW needed —
// runs on every boot from ghal_init, including QEMU.
//
// A graphics failure must never take down the OS; this test
// proves the guards hold. GPU reset is N/A (legacy ring has
// no reset mechanism — documented, not invented).
// ============================================================

// Run all cases. Logs [robust] verdict. 0 = all PASS.
int intel_robust_selftest(void);

#endif // INTEL_ROBUST_H
