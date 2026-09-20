#ifndef INTEL_TEST_COPY_H
#define INTEL_TEST_COPY_H

// ============================================================
// Intel GPU COPY test — Phase 9
//
// Standalone HW test (no KWM). Allocates src/dst buffers,
// fills src with a known pattern, submits one XY copy via
// BCS, waits, validates dst on CPU.
//
// Returns: 0 = PASS, 1 = SKIP (no BCS — builder checked),
// -1 = FAIL. Never fatal: boot continues regardless.
// ============================================================

// Run once at boot from intel_init. Logs verdict to serial.
int intel_test_copy_run(void);

#endif // INTEL_TEST_COPY_H
