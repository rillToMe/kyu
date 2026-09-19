#ifndef INTEL_GTT_H
#define INTEL_GTT_H

// ============================================================
// Intel GTT (Global Graphics Translation) — Phase 6
//
// Manages the GPU page table that maps GPU virtual addresses
// to physical pages. The GTT page table lives in system RAM
// and is pointed to by MMIO registers.
// ============================================================

#include <stdint.h>

// Maximum GPU pages in GTT (2MB / 4KB = 512 entries)
#define INTEL_GTT_MAX_ENTRIES  512

// Initialize GTT: allocate page table, program MMIO registers.
// Returns 0 on success, -1 on failure.
int intel_gtt_init(void);

// Map physical pages into GTT at a given GPU virtual address.
// Returns the GTT index of the first mapped entry, or (uint32_t)-1 on failure.
uint32_t intel_gtt_map_pages(uint64_t gpu_vaddr, const uint64_t* phys_addrs, uint32_t num_pages);

// Unmap pages from GTT (clears PTEs, flushes).
void intel_gtt_unmap(uint32_t gtt_offset, uint32_t num_pages);

// Flush GTT (invalidate GPU TLB after changes).
void intel_gtt_flush(void);

// Get GTT base physical address (for MMIO programming).
uint64_t intel_gtt_base_phys(void);

#endif // INTEL_GTT_H
