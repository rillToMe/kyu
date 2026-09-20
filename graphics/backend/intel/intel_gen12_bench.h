#ifndef INTEL_GEN12_BENCH_H
#define INTEL_GEN12_BENCH_H

// ============================================================
// Gen12 benchmark — AL-12
// Same [bench] min/avg TSC discipline as Phase-20. Numbers only,
// no acceleration claims (AL-19 interprets). Runs only on live
// submission; probe-gated so a dead engine costs one timeout.
// ============================================================

void gen12_bench_run(void);

#endif // INTEL_GEN12_BENCH_H
