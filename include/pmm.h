#ifndef PMM_H
#define PMM_H

#include <stdint.h>

// ============================================================
// Physical address type — distinct from virtual pointers.
// pmm_alloc_page() returns a PHYSICAL address, not a kernel pointer.
// Callers MUST add hhdm_offset to get a virtual (HHDM) address.
// ============================================================
typedef uint64_t phys_addr_t;

#define PHYS_NULL ((phys_addr_t)0)

#define PAGE_SIZE 4096

// Contracts
void         pmm_init_dynamic(void* memmap_entries, uint64_t entry_count);
phys_addr_t  pmm_alloc_page(void);
void         pmm_free_page(phys_addr_t addr);

// Check if address is within PMM bitmap range AND currently marked as allocated.
// Used by VMM to skip Limine/boot pages that PMM never allocated.
int          pmm_owns_page(phys_addr_t addr);

uint64_t     pmm_get_used_ram(void);
uint64_t     pmm_get_total_ram(void);
uint64_t     pmm_get_free_ram(void);
uint64_t     pmm_get_used_pages(void);
uint64_t     pmm_get_total_pages(void);

// Versi TANPA LOCK untuk jalur panic (CPU yang fault bisa sedang memegang
// pmm_lock — menunggunya = freeze tanpa BSOD). Hanya untuk statistik/diagnosa.
uint64_t     pmm_get_used_pages_nolock(void);
uint64_t     pmm_get_total_pages_nolock(void);

void         pmm_set_total_ram(uint64_t size);

#endif
