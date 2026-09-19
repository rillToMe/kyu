// ============================================================
// Intel BCS (Blitter Command Streamer) — Phase 7
// (graphics/backend/intel_bcs.c)
//
// Legacy ring path for Gen8/9/11. Gen12 needs execlists —
// init reports unavailable there instead of guessing registers
// (Agent Rule 20). CPU fallback continues either way.
//
// Ring lives in contiguous system RAM, mapped into GTT at
// INTEL_BCS_RING_GPUADDR. RING_START = GPU base, CTL bit0 =
// enable (read-modify-write, readback-verified). TAIL is a
// byte offset, 8B-aligned, masked to ring size.
// ============================================================

#include "intel_bcs.h"
#include "intel_regs.h"
#include "intel_mmio.h"
#include "intel_gtt.h"
#include "intel_backend.h"
#include "pmm.h"
#include "spinlock.h"
#include <string.h>

extern uint64_t hhdm_offset;
extern void serial_print(const char* s);

static uint32_t*  g_ring_cpu;    // HHDM pointer to ring pages
static uint64_t   g_ring_phys[INTEL_BCS_RING_PAGES];
static uint32_t   g_ring_tail;   // byte offset, SW copy
static spinlock_t g_bcs_lock = SPINLOCK_INIT;
static int        g_bcs_available = 0;

int intel_bcs_is_available(void) { return g_bcs_available; }

uint64_t intel_bcs_ring_gpu_addr(void) { return INTEL_BCS_RING_GPUADDR; }

int intel_bcs_is_busy(void) {
    if (!g_bcs_available) return 0;
    uint32_t head = intel_mmio_read32(INTEL_BCS_RING_HEAD) & (INTEL_BCS_RING_SIZE - 1);
    return head != (g_ring_tail & (INTEL_BCS_RING_SIZE - 1));
}

int intel_bcs_wait_idle(uint32_t max_spins) {
    if (!g_bcs_available) return 0;
    if (max_spins == 0) max_spins = 1000000;
    uint32_t want = g_ring_tail & (INTEL_BCS_RING_SIZE - 1);
    for (uint32_t i = 0; i < max_spins; i++) {
        uint32_t head = intel_mmio_read32(INTEL_BCS_RING_HEAD) & (INTEL_BCS_RING_SIZE - 1);
        if (head == want) return 0;
    }
    return -1;
}

static int bcs_alloc_ring(void) {
    // Allocate 8 contiguous pages (retry like GTT table).
    for (int attempt = 0; attempt < 8; attempt++) {
        phys_addr_t first = pmm_alloc_page();
        if (first == PHYS_NULL) return -1;
        int ok = 1;
        g_ring_phys[0] = first;
        for (uint32_t p = 1; p < INTEL_BCS_RING_PAGES; p++) {
            phys_addr_t pa = pmm_alloc_page();
            if (pa == PHYS_NULL || pa != first + (uint64_t)p * 4096) {
                if (pa != PHYS_NULL) pmm_free_page(pa);
                for (uint32_t q = 0; q < p; q++)
                    pmm_free_page(first + (uint64_t)q * 4096);
                ok = 0;
                break;
            }
            g_ring_phys[p] = pa;
        }
        if (ok) return 0;
    }
    return -1;
}

int intel_bcs_init(void) {
    if (g_bcs_available) return 0;

    intel_gen_t gen = intel_get_generation();
    if (gen == INTEL_GEN_UNKNOWN) {
        serial_print("[intel_bcs] unknown gen — unavailable\n");
        return -1;
    }
    if (gen >= INTEL_GEN12) {
        // Gen12 submits via execlists, not the legacy ring.
        // Blocked: needs execlist init (future phase). Fallback stays.
        serial_print("[intel_bcs] Gen12 needs execlists — unavailable\n");
        return -1;
    }

    if (bcs_alloc_ring() != 0) {
        serial_print("[intel_bcs] ring alloc failed\n");
        return -1;
    }

    g_ring_cpu = (uint32_t*)(g_ring_phys[0] + hhdm_offset);
    memset(g_ring_cpu, 0, INTEL_BCS_RING_SIZE);

    // Map ring into GTT at fixed low address.
    uint32_t idx = intel_gtt_map_pages(INTEL_BCS_RING_GPUADDR,
                                       g_ring_phys, INTEL_BCS_RING_PAGES);
    if (idx == (uint32_t)-1) {
        serial_print("[intel_bcs] GTT map failed\n");
        for (uint32_t p = 0; p < INTEL_BCS_RING_PAGES; p++)
            pmm_free_page(g_ring_phys[p]);
        return -1;
    }

    // Program ring base + enable (read-modify-write, verify enable stuck).
    intel_mmio_write32(INTEL_BCS_RING_BASE, (uint32_t)INTEL_BCS_RING_GPUADDR);
    intel_mmio_write32(INTEL_BCS_RING_HEAD, 0);
    intel_mmio_write32(INTEL_BCS_RING_TAIL, 0);
    uint32_t ctl = intel_mmio_read32(INTEL_BCS_RING_CTL);
    intel_mmio_write32(INTEL_BCS_RING_CTL, ctl | INTEL_BCS_CTL_ENABLE);
    uint32_t rb = intel_mmio_read32(INTEL_BCS_RING_CTL);
    if ((rb & INTEL_BCS_CTL_ENABLE) == 0) {
        serial_print("[intel_bcs] CTL enable rejected — unavailable\n");
        return -1;
    }
    // Abort value (guard for stale TAIL): HEAD must read 0 after reset.
    if ((intel_mmio_read32(INTEL_BCS_RING_HEAD) & (INTEL_BCS_RING_SIZE - 1)) != 0) {
        serial_print("[intel_bcs] HEAD != 0 after reset — unavailable\n");
        return -1;
    }

    g_ring_tail = 0;
    g_bcs_available = 1;
    serial_print("[intel_bcs] ring ready (legacy, Gen8-11)\n");
    return 0;
}

int intel_bcs_submit(const uint32_t* dwords, uint32_t ndwords) {
    if (!g_bcs_available || !dwords || ndwords == 0) return -1;
    if (ndwords > INTEL_BCS_RING_SIZE / 4 / 2) return -1; // never fill >half ring

    uint64_t flags = spinlock_lock_irqsave(&g_bcs_lock);

    uint32_t mask = INTEL_BCS_RING_SIZE - 1;
    uint32_t tail = g_ring_tail & mask;

    // SMP: concurrent submitters share one ring. Refuse when in-flight
    // + new work would exceed half the ring — the caller falls back to
    // CPU (all vtable ops already do). No waiting under the lock.
    uint32_t head = intel_mmio_read32(INTEL_BCS_RING_HEAD) & mask;
    uint32_t used = (tail - head) & mask;
    uint32_t odd = ndwords & 1;
    if (used + ((ndwords + odd) << 2) > INTEL_BCS_RING_SIZE / 2) {
        spinlock_unlock_irqrestore(&g_bcs_lock, flags);
        return -1;
    }
    uint32_t* ring = g_ring_cpu;

    // Copy with wrap (ring viewed as DWORD array).
    for (uint32_t i = 0; i < ndwords; i++) {
        ring[((tail >> 2) + i) & (mask >> 2)] = dwords[i];
    }
    if (odd) {
        // QW-align TAIL: pad MI_NOOP.
        ring[((tail >> 2) + ndwords) & (mask >> 2)] = INTEL_MI_NOOP;
    }
    g_ring_tail = (tail + ((ndwords + odd) << 2)) & mask;

    __asm__ volatile("" ::: "memory"); // commands visible before TAIL
    intel_mmio_write32(INTEL_BCS_RING_TAIL, g_ring_tail);

    spinlock_unlock_irqrestore(&g_bcs_lock, flags);
    return 0;
}
