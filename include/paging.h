#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "pmm.h"

// --- PAGING FLAGS ---
#define PAGING_PRESENT   (1ULL << 0)
#define PAGING_WRITABLE  (1ULL << 1)
#define PAGING_USER      (1ULL << 2)
#define PAGING_DEFAULT   (PAGING_PRESENT | PAGING_WRITABLE | PAGING_USER)

// --- FUNGSI UTAMA PAGING 64-BIT ---
void init_paging(uint32_t fb_phys_addr);

// Map: returns 1 on success, 0 on failure (alloc fail, huge page collision)
int  vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags);

// Unmap a single page: returns 1 if unmapped, 0 if not mapped
int  vmm_unmap_page(uint64_t vaddr);

// Allocate a physical page and map it to vaddr: returns 1/0
int  vmm_alloc_page(uint64_t vaddr, uint64_t flags);

// Convenience: alloc + map with default flags (RW+Present)
int  paging_map_region(uint64_t vaddr);

// Check if virtual address has a mapping (SMP-safe)
int  paging_is_mapped(uint64_t vaddr);

// Sama seperti di atas tetapi TANPA mengambil paging_lock. HANYA untuk jalur
// panic / diagnosa: CPU yang fault bisa jadi sedang memegang lock itu, dan
// menunggunya = freeze tanpa BSOD (self-deadlock).
int  paging_is_mapped_nolock(uint64_t vaddr);

// --- ADDRESS SPACE MANAGEMENT ---

// Create a new PML4 with kernel mappings cloned.
// Returns physical address of new PML4, or PHYS_NULL on failure.
phys_addr_t vmm_create_address_space(void);

// Full-copy clone of a user address space for fork() (P0 Phase 6B):
// fresh PML4 (kernel halves shared by value, never deep-copied) +
// every present 4KB user page duplicated (fresh frame, contents copied,
// identical flags). Parent tables are only read. OOM mid-clone destroys
// the partial child AS and returns PHYS_NULL; the parent is untouched.
phys_addr_t vmm_clone_user_as(phys_addr_t parent_pml4_phys);

// Destroy an entire address space:
//   - Frees all user-range pages (PML4 indices 0..255)
//   - Frees PT, PD, PDPT hierarchy pages
//   - If free_pml4=1, also frees the PML4 page itself
void vmm_destroy_address_space(phys_addr_t pml4_phys, int free_pml4);

// Switch CR3 + current_pml4 to a given PML4 physical address.
// Updates percpu CR3 tracking for the current CPU.
void vmm_switch_pml4(phys_addr_t pml4_phys);

// Switch back to the boot/kernel PML4 (saved at init_paging time).
// Always safe — even if current_pml4 was changed to a user PML4.
void vmm_switch_to_kernel_as(void);

// Get the boot kernel PML4 physical address (saved at init).
phys_addr_t vmm_get_kernel_pml4_phys(void);

// Destroy a task's address space and switch back to kernel PML4.
// Handles: save old PML4 → switch to kernel → destroy old → done.
void vmm_destroy_task_as(phys_addr_t pml4_phys);

// Map a page into the KERNEL PML4 (not current PML4).
// Used by heap expansion to ensure kernel heap pages persist across AS switches.
int  vmm_map_page_kernel(uint64_t vaddr, uint64_t paddr, uint64_t flags);

// Allocate a physical page and map it into the KERNEL PML4.
int  vmm_alloc_page_kernel(uint64_t vaddr, uint64_t flags);

// Map a page into a SPECIFIC PML4 (used for ELF loading into user AS).
int  vmm_map_page_into(uint64_t vaddr, uint64_t paddr, uint64_t flags,
                        phys_addr_t target_pml4_phys);

// Allocate a physical page and map it into a SPECIFIC PML4.
// target_pml4 == PHYS_NULL → kernel PML4. (FIX_002: target selalu eksplisit.)
int  vmm_alloc_page_into(uint64_t vaddr, uint64_t flags, phys_addr_t target_pml4);

// Check if vaddr is mapped in a SPECIFIC PML4 (SMP-safe).
// pml4_phys == PHYS_NULL → kernel PML4.
int  paging_is_mapped_into(uint64_t vaddr, phys_addr_t pml4_phys);

// Unmap vaddr from a SPECIFIC PML4. Returns the physical address of the
// unmapped page so the caller can free it (PHYS_NULL if not mapped).
// FIX_005 Tahap 3: dipakai uheap untuk sys_free region user.
phys_addr_t vmm_unmap_page_from(uint64_t vaddr, phys_addr_t target_pml4_phys);

// --- TLB MANAGEMENT ---

// Invalidate TLB for a single virtual address (current CPU)
void vmm_tlb_shootdown(uint64_t vaddr);

// Full TLB flush via CR3 reload (current CPU)
void vmm_flush_tlb_all(void);

// Read current CR3 (physical address)
phys_addr_t vmm_read_cr3(void);

#endif