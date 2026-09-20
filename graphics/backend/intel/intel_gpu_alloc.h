#ifndef INTEL_GPU_ALLOC_H
#define INTEL_GPU_ALLOC_H

// ============================================================
// Intel GPU Buffer Abstraction (Phase 5)
//
// Tracks physical backing pages, CPU virtual address (HHDM),
// GPU virtual address (GTT), and buffer lifetime.
//
// The iGPU uses system RAM — no dedicated VRAM. Buffers are
// PMM-allocated pages mapped into the GPU's address space (GTT).
// ============================================================

#include <stdint.h>
#include "intel_regs.h"

// Maximum pages per buffer (64 pages = 256 KB — generous for 2D)
#define INTEL_BUF_MAX_PAGES  64

// Maximum concurrent GPU buffers (small — 2D only, no general VRAM manager)
#define INTEL_MAX_BUFFERS    32

// GPU buffer states
typedef enum {
    INTEL_BUF_FREE = 0,        // slot unused
    INTEL_BUF_ALLOCATED,       // physical pages allocated, not yet mapped to GTT
    INTEL_BUF_MAPPED,          // mapped into GTT, GPU-accessible
    INTEL_BUF_CPU_ONLY,        // CPU-accessible only (no GPU mapping)
} intel_buf_state_t;

// GPU buffer descriptor
typedef struct {
    uint32_t           id;            // unique buffer ID
    intel_buf_state_t  state;
    uint32_t           width;         // logical width (pixels)
    uint32_t           height;        // logical height (pixels)
    uint32_t           stride;        // pixels per row (>= width)
    uint32_t           bpp;           // bits per pixel (32)
    uint64_t           size_bytes;    // total size in bytes

    // Physical backing
    uint32_t           num_pages;
    uint64_t           phys_addrs[INTEL_BUF_MAX_PAGES]; // physical page addresses

    // CPU mapping (via HHDM)
    uint32_t*          cpu_ptr;       // = phys_addrs[0] + hhdm_offset (contiguous)
    int                cpu_mapped;    // 1 = cpu_ptr valid

    // GPU mapping (via GTT)
    uint64_t           gpu_vaddr;     // GPU virtual address (page-aligned)
    uint32_t           gtt_offset;    // index into GTT page table
    int                gpu_mapped;    // 1 = gpu_vaddr valid
} intel_gpu_buffer_t;

// --- Buffer Management API ---

// Initialize the GPU buffer allocator (called once during intel_init)
void intel_gpu_alloc_init(void);

// Create a GPU buffer: allocates physical pages.
// Returns buffer ID (>= 0) or -1 on failure.
int  intel_gpu_buffer_create(uint32_t width, uint32_t height);

// Destroy a GPU buffer: frees physical pages, unmaps from GTT.
void intel_gpu_buffer_destroy(int buf_id);

// Map a buffer into the GPU address space (GTT).
// Returns 0 on success, -1 on failure.
int  intel_gpu_buffer_map_gpu(int buf_id);

// Unmap a buffer from the GPU address space.
void intel_gpu_buffer_unmap_gpu(int buf_id);

// Get buffer by ID (returns NULL if invalid/free)
intel_gpu_buffer_t* intel_gpu_buffer_get(int buf_id);

// Get CPU pointer for a buffer (for direct pixel access)
uint32_t* intel_gpu_buffer_cpu_ptr(int buf_id);

// Get GPU virtual address for a buffer
uint64_t intel_gpu_buffer_gpu_vaddr(int buf_id);

// Get buffer stride (pixels per row)
uint32_t intel_gpu_buffer_stride(int buf_id);

#endif // INTEL_GPU_ALLOC_H
