#ifndef INTEL_CMD_H
#define INTEL_CMD_H

// ============================================================
// Intel GPU Command Buffer — Phase 8
//
// Staging buffer (CPU RAM) + emit helpers for MI and XY 2D
// commands + submit/wait via BCS. One queue, synchronous.
//
// MI commands (NOOP/BB_END/STORE_IMM) are generation-stable.
// XY COPY/FILL encoding follows i915 and is HW-validated in
// Phase 9/10 — builders exist now so those phases can use them.
//
// Status page (1 PMM page, GTT @ INTEL_BCS_STATUS_GPUADDR)
// is written by MI_STORE_IMM and polled by CPU. Ready now,
// consumed by fences in Phase 13.
// ============================================================

#include <stdint.h>

#define INTEL_CMD_MAX_DWORDS 256

typedef struct {
    uint32_t dw[INTEL_CMD_MAX_DWORDS];
    uint32_t n;
} intel_cmd_t;

void intel_cmd_begin(intel_cmd_t* c);
int  intel_cmd_emit_noop(intel_cmd_t* c);
int  intel_cmd_emit_bb_end(intel_cmd_t* c);
int  intel_cmd_emit_store(intel_cmd_t* c, uint64_t gpu_addr, uint32_t value);
int  intel_cmd_emit_copy(intel_cmd_t* c, uint64_t dst_gpu, uint32_t dst_pitch,
                         uint64_t src_gpu, uint32_t src_pitch,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h);
// Phase 11: 1:1 blit with independent src/dst offsets (no scaling).
int  intel_cmd_emit_blit(intel_cmd_t* c, uint64_t dst_gpu, uint32_t dst_pitch,
                         uint64_t src_gpu, uint32_t src_pitch,
                         uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                         uint32_t w, uint32_t h);
int  intel_cmd_emit_fill(intel_cmd_t* c, uint64_t dst_gpu, uint32_t dst_pitch,
                         uint32_t color,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Submit staging to BCS ring. 0 = submitted, -1 = fallback/no BCS.
int intel_cmd_submit(intel_cmd_t* c);
// Wait until ring idle. 0 = idle, -1 = timeout/unavailable.
int intel_cmd_wait(uint32_t max_spins);

// Status page (for MI_STORE + Phase 13). 0 ok, -1 fail.
int      intel_cmd_status_init(void);
uint64_t intel_cmd_status_gpu_addr(void);
uint32_t intel_cmd_status_read(void);
void     intel_cmd_status_write(uint32_t v);

// Builder-only self-test (no HW touch). 0 = PASS.
int intel_cmd_selftest(void);

#endif // INTEL_CMD_H
