#ifndef INTEL_GEN12_SUBMIT_H
#define INTEL_GEN12_SUBMIT_H

#include <stdint.h>

// ============================================================
// Gen12 minimal submission — AL-8
// (graphics/backend/intel/intel_gen12_submit.h)
//
// One serialized owner (own lock): forcewake GT → write ring →
// patch image TAIL → ELSP submit → fence wait. First op is a
// STORE-to-status (proves execution, no blitter needed).
// All failures bounded, all reports honest (PASS/SKIP/BLOCKED).
// ============================================================

// GGTT reservation for the execlist ring (16KB). Map so far:
//   0x00000 legacy ring | 0x10000 legacy status | 0x20000 gen12 ctx
//   0x24000 gen12 status | 0x25000 gen12 ring | 0x100000+ bump area
#define GEN12_RING_GPUADDR 0x25000ULL
#define GEN12_RING_PAGES   4
#define GEN12_RING_SIZE    (GEN12_RING_PAGES * 4096u)

// Map the ring (idempotent). 0 ok, -1 SKIP/FAIL (prints reason).
int gen12_submit_init(void);
// Minimal op: STORE fence-seqno to the status page via the ring,
// submit through ELSP, wait. Prints PASS / SKIP / BLOCKED.
// 0 = GPU executed; -1 = not proven (never fatal).
int gen12_submit_store_test(void);
// AL-15: THE single submission owner. Copies ndw DWORDs from caller
// memory into the ring and submits — staging, mirror-sync, image
// patch and ELSP all under one irqsave lock. Callers never touch
// the ring; concurrent submitters serialize here, never corrupt.
// No wrap (ring full → -1 BLOCKED). 0 submitted.
int gen12_submit_commit(const uint32_t* dw, uint32_t ndw);
// AL-15: live-gated SMP ordering proof (two owners → ordered commits,
// intact regions, both fences). Prints PASS / SKIP, never fatal.
void gen12_submit_smp_selftest(void);
// AL-14: dispatch counters (HW-done vs CPU-fallback in vtable ops).
void gen12_count_hw(void);
void gen12_count_fb(void);
void gen12_op_counts(uint64_t* hw, uint64_t* fb);
// AL-13: 1 after the boot STORE proved ELSP+context+fence on HW.
// Gates GHAL dispatch; cleared on the first HW failure (fail-fast
// to CPU, no per-frame timeout stalls).
int gen12_is_live(void);
void gen12_note_hw_failure(void);

#endif // INTEL_GEN12_SUBMIT_H
