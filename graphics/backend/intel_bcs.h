#ifndef INTEL_BCS_H
#define INTEL_BCS_H

// ============================================================
// Intel BCS (Blitter Command Streamer) — Phase 7
//
// Owns the BCS ring buffer: allocation (system RAM, GPU-visible
// via GTT), ring enable, raw DWORD submission, idle polling.
//
// Single queue, synchronous. No scheduler, no batching.
// If init fails (unsupported gen, MMIO mismatch) the backend
// stays in CPU-fallback mode — never fatal.
//
// GTT layout (low region, buffers start at 1MB):
//   0x00000-0x07FFF  BCS ring (32KB)
//   0x10000-0x10FFF  status page (seqno, Phase 13 fences)
// ============================================================

#include <stdint.h>

#define INTEL_BCS_RING_PAGES   8
#define INTEL_BCS_RING_SIZE    (INTEL_BCS_RING_PAGES * 4096)
#define INTEL_BCS_RING_GPUADDR 0x00000ULL
#define INTEL_BCS_STATUS_GPUADDR 0x10000ULL

// Init BCS ring. 0 = available, -1 = unavailable (fallback).
int intel_bcs_init(void);

// 1 when ring enabled + verified, else 0.
int intel_bcs_is_available(void);

// 1 while GPU HEAD != submitted TAIL.
int intel_bcs_is_busy(void);

// Submit DWORDs to ring. Copies with wrap, bumps TAIL.
// Pads to QW with MI_NOOP when ndwords is odd.
// Returns 0 on submit, -1 when unavailable/bad input.
int intel_bcs_submit(const uint32_t* dwords, uint32_t ndwords);

// Spin until HEAD == TAIL or spins exhausted. 0 = idle.
int intel_bcs_wait_idle(uint32_t max_spins);

// GPU address of ring base (for diagnostics).
uint64_t intel_bcs_ring_gpu_addr(void);

#endif // INTEL_BCS_H
