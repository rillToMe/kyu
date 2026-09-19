#ifndef INTEL_GEN12_TEST_FILL_H
#define INTEL_GEN12_TEST_FILL_H

// ============================================================
// Gen12 hardware FILL tests — AL-10
// full / small / edge / unaligned / multi-in-one-batch.
// Same pipeline + verdict discipline as the AL-9 COPY tests.
// ============================================================

void gen12_test_fill_run(void);

#endif // INTEL_GEN12_TEST_FILL_H
