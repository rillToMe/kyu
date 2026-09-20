#ifndef INTEL_GEN12_BATCH_H
#define INTEL_GEN12_BATCH_H

#include <stdint.h>

// ============================================================
// Gen12 batch buffer — AL-6
// (graphics/backend/intel/intel_gen12_batch.h)
//
// One-page GGTT-mapped batch, cursor emit, sealed by BB_END.
// COPY/FILL/BLIT only. No submission here (AL-8), no GHAL (AL-13).
// ============================================================

#define GEN12_BATCH_DWORDS 1024

typedef struct {
    uint32_t* cpu;        // HHDM write pointer (scratch or mapped page)
    uint64_t  phys;       // backing page (0 = scratch, not owned)
    uint64_t  gpu;        // GGTT address of backing page (0 = scratch)
    uint32_t  n;          // DWORDs emitted
    int       sealed;     // 1 after end()
    int       backed;     // 1 = owns phys+mapping
} gen12_batch_t;

// Allocate + GGTT-map one page. 0 ok, -1 SKIP/FAIL (prints reason).
int gen12_batch_begin(gen12_batch_t* b);
// Release backing (idempotent; scratch batches need no call but tolerate it).
void gen12_batch_free(gen12_batch_t* b);

// Emitters take PRE-VALIDATED geometry (Phase-12 owns clipping at the
// GHAL layer). Pitch in pixels; addresses GGTT, 4K-aligned page base
// checked in-window. Each returns 0 or -1 (sealed/full/bad geom).
int gen12_batch_emit_copy(gen12_batch_t* b, uint64_t dst, uint32_t dst_px,
                          uint64_t src, uint32_t src_px,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int gen12_batch_emit_fill(gen12_batch_t* b, uint64_t dst, uint32_t dst_px,
                          uint32_t color,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int gen12_batch_emit_blit(gen12_batch_t* b, uint64_t dst, uint32_t dst_px,
                          uint64_t src, uint32_t src_px,
                          uint32_t sx, uint32_t sy,
                          uint32_t dx, uint32_t dy,
                          uint32_t w, uint32_t h);
// Terminate with MI_BATCH_BUFFER_END. Seals the batch.
int gen12_batch_end(gen12_batch_t* b);
// GGTT address of the batch start (for AL-8 submit). 0 when scratch/empty.
uint64_t gen12_batch_gpu_addr(const gen12_batch_t* b);
// CPU-only structural self-test + live backed-batch test when GTT is up.
void gen12_batch_selftest(void);

#endif // INTEL_GEN12_BATCH_H
