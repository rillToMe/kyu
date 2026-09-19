#ifndef INTEL_BENCH_H
#define INTEL_BENCH_H

// ============================================================
// GPU benchmark — Phase 20
//
// Times CPU fallback ops, command generation, damage-upload
// patterns and (when BCS is live) HW submit+exec+sync — all in
// raw TSC cycles (60Hz timer ticks are far too coarse for
// µs-scale ops; TSC rate is uncalibrated so only compare
// within one boot). Always safe: kmalloc failure or missing
// GTT degrades to fewer lines, never a crash.
//
// Runs once from ghal_init (all machines, incl. QEMU without
// iGPU). HW columns fill in on Gen8-11 real hardware.
// ============================================================

// Run all benchmarks. Logs [bench] lines to serial.
void intel_bench_run(void);

#endif // INTEL_BENCH_H
