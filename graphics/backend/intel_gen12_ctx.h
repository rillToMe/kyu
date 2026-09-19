#ifndef INTEL_GEN12_CTX_H
#define INTEL_GEN12_CTX_H

#include <stdint.h>

// ============================================================
// Gen12 BCS0 Logical Ring Context — AL-5
// (graphics/backend/intel_gen12_ctx.h)
//
// One static context instance (single serialized 2D owner).
// Allocation + GGTT mapping + register image + validation +
// lifecycle. NO submission here (AL-8); NO ring yet (AL-8).
// ============================================================

// GGTT reservation for the context image (4 pages). Map:
//   0x00000 legacy BCS ring (32KB) | 0x10000 status page
//   0x20000 gen12 context (16KB)   | 0x100000+ buffer bump area
#define GEN12_CTX_GPUADDR  0x20000ULL
#define GEN12_CTX_PAGES    4

// 0 = ready (LRCA valid); -1 = SKIP/BLOCKED (see serial lines).
int intel_gen12_ctx_create(void);
// Idempotent teardown. Safe to call when never created.
void intel_gen12_ctx_destroy(void);
// GGTT address of the context (LRCA base = PPHWSP page). 0 = none.
uint64_t intel_gen12_ctx_lrca(void);
// Re-check a built image. 0 = valid.
int intel_gen12_ctx_validate(const uint32_t* state);
// AL-8: patch the ring registers in the state image (START/HEAD/TAIL/CTL).
// Mirrors lrc_update_regs() minus render/WA parts. 0 ok, -1 when down.
int intel_gen12_ctx_set_ring(uint64_t ring_gpu, uint32_t ring_size,
                             uint32_t tail_bytes);
// AL-9: program the PPGTT root (PDP0 = PD physical; PDP1-3 stay 0).
// Mirrors ASSIGN_CTX_PDP (physical addresses). 0 ok, -1 when down.
int intel_gen12_ctx_set_pdp(uint64_t pd_phys);

#endif // INTEL_GEN12_CTX_H
