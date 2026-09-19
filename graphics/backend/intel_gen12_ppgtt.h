#ifndef INTEL_GEN12_PPGTT_H
#define INTEL_GEN12_PPGTT_H

#include <stdint.h>

// ============================================================
// Gen12 minimal PPGTT (alias of the GGTT window) — AL-9
// (graphics/backend/intel_gen12_ppgtt.h)
//
// XY commands resolve addresses through the context VM (the CTX
// PDP slots), not GGTT. This module builds the smallest valid
// 3-level VM covering our 2MB GGTT window:
//
//   CTX_PDP0 (in image) → PD[0] → PT[0..511] → data pages
//   PDP1-3 = 0 (never walked below 1GB)
//
// The PT mirrors the live GGTT PTEs (same phys, PRESENT|RW, PAT0)
// re-synced before every Gen12 submit that carries XY commands.
// Table pages are CPU-written + clflushed; the walker reads DRAM.
// ============================================================

// Build PD+PT, program PDP0. Idempotent. 0 ok, -1 SKIP/BLOCKED/FAIL.
int gen12_ppgtt_init(void);
// Re-mirror GGTT[0..511] into the PT + clflush. Call before XY submits.
int gen12_ppgtt_sync(void);
// Write back + invalidate CPU cache lines over [phys, phys+len).
// Needed around GPU access (pattern write before, dst read after).
void gen12_cpu_clflush(uint64_t phys, uint64_t len);

#endif // INTEL_GEN12_PPGTT_H
