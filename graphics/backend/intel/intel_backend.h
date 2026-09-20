#ifndef INTEL_BACKEND_H
#define INTEL_BACKEND_H

#include "ghal.h"
#include "intel_regs.h"

// GHAL backend vtable (defined in intel_init.c)
extern const ghal_backend_ops_t intel_backend_ops;

// Scanout framebuffer stash (via ghal_set_framebuffer fan-out).
void intel_backend_set_fb(uint32_t* fb, uint32_t w, uint32_t h, uint32_t pitch_bytes);

// Intel GPU state accessors (for blitter module)
intel_gen_t intel_get_generation(void);
uint16_t    intel_get_device_id(void);
int         intel_is_active(void);

// Phase 7/8 modules (ring + command buffer, used by Phase 9+ tests)
#include "intel_bcs.h"
#include "intel_cmd.h"
#include "intel_test_copy.h"
#include "intel_test_fill.h"
#include "intel_test_blit.h"
#include "intel_rect.h"
#include "intel_fence.h"
#include "intel_irq.h"
#include "intel_surface.h"
#include "intel_bench.h"
#include "intel_robust.h"

// --- Phase 23: SMP locking protocol (audit, all SMP-safe) ---
// One GPU submission owner: g_bcs_lock serializes every ring submit.
// Staging (intel_cmd_t) is caller-owned (stack) — CPUs build lock-free,
// submission is explicit and serialized.
//
//   g_buf_lock   : buffer pool, id alloc, gpu_vaddr bump (irqsave)
//   g_gtt_lock   : PTE writes + g_gtt_used bump (irqsave)
//   g_bcs_lock   : ring copy + TAIL bump + free-space check (irqsave)
//   g_fence_lock : seqno alloc only (irqsave)
//   status page  : single-writer GPU, lock-free multi-reader CPU
//   irq snapshot : volatile u32, lock-free (atomic aligned R/W on x86-64)
//
// Discipline: irqsave on every GPU lock (submit may race ISR-side
// observation); never hlt/sleep under a GPU lock (fence_wait_sleep runs
// lock-free); ring-full degrades to CPU fallback (-1), never overwrites
// in-flight commands. kmalloc paths rely on the locked kernel heap.
// Verified by clean -smp 8 boots (bench + robust PASS, no panic).

#endif // INTEL_BACKEND_H
