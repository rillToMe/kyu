// ============================================================
// Intel GTT (Global Graphics Translation) — Phase 6
// (graphics/backend/intel_gtt.c)
//
// Manages the GPU page table. The GTT page table is an array of
// PTEs (Page Table Entries) in system RAM, pointed to by MMIO
// registers. Each PTE maps a GPU virtual address to a physical page.
//
// For 2D-only acceleration, we use a simple flat page table with
// 4KB pages. No fancy per-process GTT — one global table for all
// GPU buffers.
// ============================================================

#include "intel_gtt.h"
#include "intel_regs.h"
#include "intel_mmio.h"
#include "intel_backend.h"
#include "pmm.h"
#include "heap.h"
#include "spinlock.h"
#include <string.h>

extern uint64_t hhdm_offset;
extern void serial_print(const char* s);

// GTT page table in system RAM (physically contiguous)
static uint32_t* g_gtt_table;        // CPU pointer to GTT PTEs
static uint64_t   g_gtt_table_phys;   // Physical address of GTT table
static uint32_t   g_gtt_entries;      // Number of PTE slots
static uint32_t   g_gtt_used;         // Next free PTE index
static spinlock_t g_gtt_lock = SPINLOCK_INIT;
static int        g_gtt_active = 0;

// Size of GTT in bytes = entries * 4KB per entry
#define GTT_SIZE_BYTES  (g_gtt_entries * 4096ULL)

int intel_gtt_init(void) {
    if (g_gtt_active) return 0;

    // Determine GPU generation for PTE format
    intel_gen_t gen = intel_get_generation();

    // Allocate GTT page table: 512 entries = 2MB GPU address space
    // This is a 4KB-aligned array of 32-bit PTEs (Gen8-11) or
    // 64-bit PTEs (Gen12, stored as pairs of 32-bit DWORDs).
    g_gtt_entries = INTEL_GTT_MAX_ENTRIES;

    // Table size: for Gen12 we need 8 bytes per entry, for Gen8-11 we need 4.
    // Use 8 bytes per entry universally (Gen12 compatible, Gen8-11 ignores HI DWORD).
    uint32_t table_size = g_gtt_entries * 8;

    // Allocate contiguous physical pages for the table
    uint32_t num_table_pages = (table_size + 4095) / 4096;
    phys_addr_t table_phys = PHYS_NULL;
    for (int attempt = 0; attempt < 8; attempt++) {
        phys_addr_t first = pmm_alloc_page();
        if (first == PHYS_NULL) {
            serial_print("[intel_gtt] failed to allocate GTT table page\n");
            return -1;
        }

        // Check contiguity for remaining pages
        int ok = 1;
        for (uint32_t p = 1; p < num_table_pages; p++) {
            phys_addr_t pa = pmm_alloc_page();
            if (pa == PHYS_NULL || pa != first + (uint64_t)p * 4096) {
                // Not contiguous — free everything and retry
                if (pa != PHYS_NULL) pmm_free_page(pa);
                for (uint32_t q = 0; q < p; q++) {
                    pmm_free_page(first + (uint64_t)q * 4096);
                }
                ok = 0;
                break;
            }
        }
        if (ok) {
            table_phys = first;
            break;
        }
    }

    if (table_phys == PHYS_NULL) {
        serial_print("[intel_gtt] cannot allocate contiguous GTT table\n");
        return -1;
    }

    g_gtt_table_phys = table_phys;
    g_gtt_table = (uint32_t*)(table_phys + hhdm_offset);

    // Zero the entire table
    memset(g_gtt_table, 0, table_size);

    // Program MMIO registers to point to the GTT table
    // GTT_BASE_LOW: physical address bits [31:12], shifted left by 12
    // The register format is: base >> 12 (hardware shifts back)
    intel_mmio_write32(INTEL_GTT_BASE_LOW, (uint32_t)(table_phys >> 12));

    // GTT_BASE_HIGH: bits [63:32] (Gen12 only, Gen8-11 ignore this)
    if (gen >= INTEL_GEN12) {
        intel_mmio_write32(INTEL_GTT_BASE_HIGH, (uint32_t)(table_phys >> 32));
    }

    // GTT_SIZE: total GPU address space in bytes, stored as size >> 10
    // (hardware multiplies by 1024)
    uint32_t gtt_size_reg = (uint32_t)(GTT_SIZE_BYTES >> 10);
    intel_mmio_write32(INTEL_GTT_SIZE, gtt_size_reg);

    // GTT_MEM_CTRL: enable GTT paging
    uint32_t ctrl = intel_mmio_read32(INTEL_GTT_MEM_CTRL);
    ctrl |= INTEL_GTT_MEM_ENABLE;
    intel_mmio_write32(INTEL_GTT_MEM_CTRL, ctrl);

    // Flush any stale GPU TLB entries
    intel_gtt_flush();

    g_gtt_used = 0;
    g_gtt_active = 1;

    serial_print("[intel_gtt] initialized: ");
    // Simple number print
    { char buf[16]; int pos = 0;
      uint32_t n = g_gtt_entries;
      if (n == 0) { buf[pos++] = '0'; }
      else { char tmp[10]; int t = 0;
        while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
        while (t > 0) { buf[pos++] = tmp[--t]; }
      }
      buf[pos++] = ' '; buf[pos++] = 'e'; buf[pos++] = 'n'; buf[pos++] = 't';
      buf[pos++] = 'r'; buf[pos++] = 'i'; buf[pos++] = 'e'; buf[pos++] = 's';
      buf[pos] = 0;
      serial_print(buf); }
    serial_print("\n");

    return 0;
}

uint32_t intel_gtt_map_pages(uint64_t gpu_vaddr, const uint64_t* phys_addrs, uint32_t num_pages) {
    if (!g_gtt_active || !phys_addrs || num_pages == 0) return (uint32_t)-1;

    uint64_t flags = spinlock_lock_irqsave(&g_gtt_lock);

    // HW truth: GTT entry N covers GPU page N (vaddr = N*4KB).
    // Honor the caller's vaddr so PTE index matches the address
    // the GPU will actually use. vaddr==0 keeps legacy bump.
    uint32_t start;
    if (gpu_vaddr != 0) {
        if (gpu_vaddr & 0xFFF) {
            spinlock_unlock_irqrestore(&g_gtt_lock, flags);
            return (uint32_t)-1;
        }
        start = (uint32_t)(gpu_vaddr >> 12);
        if (start + num_pages > g_gtt_entries) {
            spinlock_unlock_irqrestore(&g_gtt_lock, flags);
            return (uint32_t)-1;
        }
    } else {
        // Legacy bump path.
        start = g_gtt_used;
        if (start + num_pages > g_gtt_entries) {
        // Wrap around and search from beginning
        start = 0;
        uint32_t free_count = 0;
        for (uint32_t i = 0; i < g_gtt_entries; i++) {
            // Check if PTE is free (low bit == 0 for Gen8-11, both DWORDs == 0 for Gen12)
            if (g_gtt_table[i * 2] == 0 && g_gtt_table[i * 2 + 1] == 0) {
                if (free_count == 0) start = i;
                free_count++;
                if (free_count == num_pages) break;
            } else {
                free_count = 0;
            }
        }
        if (free_count < num_pages) {
            spinlock_unlock_irqrestore(&g_gtt_lock, flags);
            return (uint32_t)-1;
        }
        }
    }

    // Map each page into the GTT
    intel_gen_t gen = intel_get_generation();
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t idx = start + i;
        uint64_t phys = phys_addrs[i];

        if (gen >= INTEL_GEN12) {
            // Gen12: 64-bit PTE (2 DWORDs per entry)
            uint64_t pte = (phys & ~0xFFFULL) | INTEL_G12_GTT_PTE_VALID;
            g_gtt_table[idx * 2]     = (uint32_t)(pte & 0xFFFFFFFF);
            g_gtt_table[idx * 2 + 1] = (uint32_t)(pte >> 32);
        } else {
            // Gen8-11: 32-bit PTE
            uint32_t pte = (uint32_t)((phys & ~0xFFFULL) | INTEL_G8_GTT_PTE_VALID);
            g_gtt_table[idx * 2] = pte;
            g_gtt_table[idx * 2 + 1] = 0;
        }
    }

    if (start + num_pages > g_gtt_used) g_gtt_used = start + num_pages;
    spinlock_unlock_irqrestore(&g_gtt_lock, flags);

    // Flush GPU TLB after PTE changes
    intel_gtt_flush();

    return start;
}

void intel_gtt_unmap(uint32_t gtt_offset, uint32_t num_pages) {
    if (!g_gtt_active) return;

    uint64_t flags = spinlock_lock_irqsave(&g_gtt_lock);
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t idx = gtt_offset + i;
        if (idx < g_gtt_entries) {
            g_gtt_table[idx * 2] = 0;
            g_gtt_table[idx * 2 + 1] = 0;
        }
    }
    spinlock_unlock_irqrestore(&g_gtt_lock, flags);

    intel_gtt_flush();
}

void intel_gtt_flush(void) {
    if (!g_gtt_active) return;

    // Trigger GTT invalidation via MMIO
    // Write 1 to INTEL_GTT_INV bit 0 to flush GPU TLB
    intel_mmio_write32(INTEL_GTT_INV, INTEL_GTT_INV_TRIGGER);

    // Brief delay for invalidation to complete
    // On real hardware, poll GTT_INV for completion; for now, a small spin.
    for (volatile int i = 0; i < 100; i++) { __asm__ volatile("nop"); }
}

uint64_t intel_gtt_base_phys(void) {
    return g_gtt_table_phys;
}

int intel_gtt_is_active(void) { return g_gtt_active; }

// AL-9: read-only snapshot of one PTE (both DWORDs). 0 when down/OOB.
uint64_t intel_gtt_pte_raw(uint32_t idx) {
    if (!g_gtt_active || idx >= g_gtt_entries) return 0;
    uint64_t lo = g_gtt_table[idx * 2];
    uint64_t hi = g_gtt_table[idx * 2 + 1];
    return (hi << 32) | lo;
}
