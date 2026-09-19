// ============================================================
// Intel GPU Buffer Allocator (Phase 5)
// (graphics/backend/intel_gpu_alloc.c)
//
// Manages GPU buffers: physical page allocation, CPU mapping
// (via HHDM), and GPU virtual address assignment.
// ============================================================

#include "intel_gpu_alloc.h"
#include "intel_gtt.h"
#include "pmm.h"
#include "heap.h"
#include "spinlock.h"
#include <string.h>

extern uint64_t hhdm_offset;
extern void serial_print(const char* s);

static intel_gpu_buffer_t g_buffers[INTEL_MAX_BUFFERS];
static uint32_t g_next_buf_id = 1;
static spinlock_t g_buf_lock = SPINLOCK_INIT;

// Bump allocator for GPU virtual addresses
static uint64_t g_next_gpu_vaddr = 0;
#define GPU_VADDR_BASE  (1ULL * 1024 * 1024)
#define GPU_VADDR_MAX   (256ULL * 1024 * 1024)

void intel_gpu_alloc_init(void) {
    memset(g_buffers, 0, sizeof(g_buffers));
    g_next_buf_id = 1;
    g_next_gpu_vaddr = GPU_VADDR_BASE;
    serial_print("[intel_gpu] buffer allocator init\n");
}

static intel_gpu_buffer_t* find_free_slot(void) {
    for (int i = 0; i < INTEL_MAX_BUFFERS; i++) {
        if (g_buffers[i].state == INTEL_BUF_FREE) return &g_buffers[i];
    }
    return NULL;
}

int intel_gpu_buffer_create(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > 8192 || height > 8192) return -1;

    uint64_t bytes = (uint64_t)width * height * 4;
    if (bytes == 0 || (bytes / 4 / width) != height) return -1;

    uint32_t num_pages = (uint32_t)((bytes + 4095) / 4096);
    if (num_pages > INTEL_BUF_MAX_PAGES) return -1;

    uint64_t flags = spinlock_lock_irqsave(&g_buf_lock);
    intel_gpu_buffer_t* buf = find_free_slot();
    if (!buf) {
        spinlock_unlock_irqrestore(&g_buf_lock, flags);
        return -1;
    }

    // Allocate physical pages
    uint32_t allocated = 0;
    for (uint32_t i = 0; i < num_pages; i++) {
        phys_addr_t pa = pmm_alloc_page();
        if (pa == PHYS_NULL) break;
        memset((void*)(pa + hhdm_offset), 0, 4096);
        buf->phys_addrs[i] = pa;
        allocated++;
    }

    if (allocated == 0) {
        spinlock_unlock_irqrestore(&g_buf_lock, flags);
        return -1;
    }

    // Check contiguity for linear CPU access
    int contiguous = 1;
    for (uint32_t i = 1; i < allocated; i++) {
        if (buf->phys_addrs[i] != buf->phys_addrs[i - 1] + 4096) {
            contiguous = 0;
            break;
        }
    }

    buf->id = g_next_buf_id++;
    buf->state = INTEL_BUF_ALLOCATED;
    buf->width = width;
    buf->height = height;
    buf->stride = width;
    buf->bpp = 32;
    buf->size_bytes = bytes;
    buf->num_pages = allocated;
    buf->gpu_mapped = 0;
    buf->gpu_vaddr = 0;
    buf->gtt_offset = 0;

    if (contiguous) {
        buf->cpu_ptr = (uint32_t*)(buf->phys_addrs[0] + hhdm_offset);
        buf->cpu_mapped = 1;
    } else {
        buf->cpu_ptr = NULL;
        buf->cpu_mapped = 0;
    }

    int id = (int)buf->id;
    spinlock_unlock_irqrestore(&g_buf_lock, flags);
    return id;
}

void intel_gpu_buffer_destroy(int buf_id) {
    if (buf_id <= 0) return;

    uint64_t flags = spinlock_lock_irqsave(&g_buf_lock);
    for (int i = 0; i < INTEL_MAX_BUFFERS; i++) {
        if (g_buffers[i].id == (uint32_t)buf_id &&
            g_buffers[i].state != INTEL_BUF_FREE) {
            intel_gpu_buffer_t* buf = &g_buffers[i];

            if (buf->gpu_mapped) {
                intel_gtt_unmap(buf->gtt_offset, buf->num_pages);
            }

            for (uint32_t p = 0; p < buf->num_pages; p++) {
                if (buf->phys_addrs[p] != 0) {
                    pmm_free_page(buf->phys_addrs[p]);
                    buf->phys_addrs[p] = 0;
                }
            }

            buf->state = INTEL_BUF_FREE;
            buf->cpu_ptr = NULL;
            buf->cpu_mapped = 0;
            buf->gpu_mapped = 0;
            break;
        }
    }
    spinlock_unlock_irqrestore(&g_buf_lock, flags);
}

int intel_gpu_buffer_map_gpu(int buf_id) {
    if (buf_id <= 0) return -1;

    uint64_t flags = spinlock_lock_irqsave(&g_buf_lock);
    for (int i = 0; i < INTEL_MAX_BUFFERS; i++) {
        if (g_buffers[i].id == (uint32_t)buf_id &&
            g_buffers[i].state == INTEL_BUF_ALLOCATED) {
            intel_gpu_buffer_t* buf = &g_buffers[i];

            uint64_t vaddr = g_next_gpu_vaddr;
            uint64_t end = vaddr + (uint64_t)buf->num_pages * 4096;
            if (end > GPU_VADDR_MAX) {
                spinlock_unlock_irqrestore(&g_buf_lock, flags);
                return -1;
            }

            uint32_t gtt_idx = intel_gtt_map_pages(
                vaddr, buf->phys_addrs, buf->num_pages);
            if (gtt_idx == (uint32_t)-1) {
                spinlock_unlock_irqrestore(&g_buf_lock, flags);
                return -1;
            }

            g_next_gpu_vaddr = end;
            buf->gpu_vaddr = vaddr;
            buf->gtt_offset = gtt_idx;
            buf->gpu_mapped = 1;
            buf->state = INTEL_BUF_MAPPED;

            spinlock_unlock_irqrestore(&g_buf_lock, flags);
            return 0;
        }
    }
    spinlock_unlock_irqrestore(&g_buf_lock, flags);
    return -1;
}

void intel_gpu_buffer_unmap_gpu(int buf_id) {
    if (buf_id <= 0) return;

    uint64_t flags = spinlock_lock_irqsave(&g_buf_lock);
    for (int i = 0; i < INTEL_MAX_BUFFERS; i++) {
        if (g_buffers[i].id == (uint32_t)buf_id &&
            g_buffers[i].state == INTEL_BUF_MAPPED) {
            intel_gpu_buffer_t* buf = &g_buffers[i];
            intel_gtt_unmap(buf->gtt_offset, buf->num_pages);
            buf->gpu_mapped = 0;
            buf->gpu_vaddr = 0;
            buf->state = INTEL_BUF_ALLOCATED;
            break;
        }
    }
    spinlock_unlock_irqrestore(&g_buf_lock, flags);
}

intel_gpu_buffer_t* intel_gpu_buffer_get(int buf_id) {
    if (buf_id <= 0) return NULL;
    for (int i = 0; i < INTEL_MAX_BUFFERS; i++) {
        if (g_buffers[i].id == (uint32_t)buf_id &&
            g_buffers[i].state != INTEL_BUF_FREE) {
            return &g_buffers[i];
        }
    }
    return NULL;
}

uint32_t* intel_gpu_buffer_cpu_ptr(int buf_id) {
    intel_gpu_buffer_t* buf = intel_gpu_buffer_get(buf_id);
    return buf ? buf->cpu_ptr : NULL;
}

uint64_t intel_gpu_buffer_gpu_vaddr(int buf_id) {
    intel_gpu_buffer_t* buf = intel_gpu_buffer_get(buf_id);
    return buf ? buf->gpu_vaddr : 0;
}

uint32_t intel_gpu_buffer_stride(int buf_id) {
    intel_gpu_buffer_t* buf = intel_gpu_buffer_get(buf_id);
    return buf ? buf->stride : 0;
}
