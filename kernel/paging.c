// ============================================================
// kernel/paging.c — Virtual Memory Manager, Kyuzen OS
//
// SMP-SAFE: All page table operations protected by paging_lock.
// ERROR-RETURNING: vmm_map_page/vmm_unmap_page return int status.
// FULL CLEANUP: vmm_destroy_address_space frees entire hierarchy.
// PER-PROCESS: vmm_create_address_space clones kernel mappings.
//
// ARCHITECTURE:
//   - 4-level page table walk (PML4 → PDPT → PD → PT)
//   - HHDM for accessing page tables (phys + hhdm_offset)
//   - Kernel mappings in PML4[256..511] (higher-half)
//   - User mappings in PML4[0..255] (lower-half)
// ============================================================

#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "spinlock.h"
#include "smp.h"
#include <stdint.h>

// HHDM offset dari kernel.c: physical P accessible di hhdm_offset + P
extern uint64_t hhdm_offset;

// Macro konversi: physical address → virtual address via HHDM
#define PHYS_TO_VIRT(phys) ((uint64_t)(phys) + hhdm_offset)
// Inverse: virtual (HHDM) → physical
#define VIRT_TO_PHYS(virt) ((uint64_t)(virt) - hhdm_offset)

// Current PML4: ALWAYS the kernel PML4. Never changes after init.
uint64_t* current_pml4 = 0;

// Boot kernel PML4 physical address — saved at init, never changes.
// Used to restore kernel AS when returning from user processes.
static phys_addr_t kernel_pml4_phys = PHYS_NULL;

// SMP-safe lock for all page table operations
static spinlock_t paging_lock = SPINLOCK_INIT;

// Forward decls (definitions below): used by init_paging pre-population.
static uint64_t* alloc_page_virt(void);
extern void kernel_panic(const char* title, const char* desc, uint64_t code);

// ============================================================
// TLB MANAGEMENT
// ============================================================

void vmm_tlb_shootdown(uint64_t vaddr) {
    // Current CPU only — future: IPI to other CPUs sharing this AS
    __asm__ volatile("invlpg (%0)" ::"r"(vaddr) : "memory");
}

void vmm_flush_tlb_all(void) {
    __asm__ volatile(
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        ::: "rax", "memory"
    );
}

phys_addr_t vmm_read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (phys_addr_t)(cr3 & 0xFFFFFFFFFFFFF000ULL);
}

// ============================================================
// INIT
// ============================================================

void init_paging(uint32_t unused) {
    (void)unused;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    uint64_t phys_pml4 = cr3 & 0xFFFFFFFFFFFFF000ULL;
    current_pml4 = (uint64_t*)PHYS_TO_VIRT(phys_pml4);
    kernel_pml4_phys = (phys_addr_t)phys_pml4;  // Save boot PML4 forever

    // Pre-populate ALL absent higher-half PML4 entries [256..511] with empty,
    // shared PDPTs. vmm_create_address_space() shallow-copies PML4[256..511]
    // at fork time; if a heap/kernel slot (e.g. PML4[288] = heap zone
    // 0xFFFF900000000000) is still absent when an AS is cloned, that AS keeps a
    // stale entry. A later expand_heap() then allocates a *fresh* PDPT in the
    // live AS while the kernel PML4 keeps its own — the same heap vaddr resolves
    // to two different physical frames across address spaces, and the PMM
    // re-hands one frame to the compositor. Installing the top-level PDPTs ONCE
    // up front (PMM is already initialized here, still single-CPU at boot) makes
    // every clone share these tables, so all later PD/PT growth is visible in
    // every address space.
    for (int i = 256; i < 512; i++) {
        if (current_pml4[i] & 1) continue;  // keep Limine's existing entries
        uint64_t* pdpt = alloc_page_virt();
        if (!pdpt) {
            kernel_panic("PAGING INIT",
                         "OOM pre-populating higher-half PDPT", (uint64_t)i);
        }
        current_pml4[i] = VIRT_TO_PHYS(pdpt) | 7;
    }
}

// ============================================================
// HELPERS (caller MUST hold paging_lock)
// ============================================================

static uint64_t* alloc_page_virt(void) {
    phys_addr_t phys = pmm_alloc_page();
    if (phys == PHYS_NULL) return 0;
    uint64_t* virt = (uint64_t*)PHYS_TO_VIRT(phys);
    memset(virt, 0, 4096);
    return virt;
}

// Page table index extraction
#define PML4_IDX(va) (((va) >> 39) & 0x1FF)
#define PDPT_IDX(va) (((va) >> 30) & 0x1FF)
#define PD_IDX(va)   (((va) >> 21) & 0x1FF)
#define PT_IDX(va)   (((va) >> 12) & 0x1FF)
#define PAGE_MASK     0xFFFFFFFFFFFFF000ULL

// ============================================================
// vmm_map_page_into — Map vaddr → paddr in a SPECIFIC PML4
//
// Used to map ELF pages into a user PML4 without changing
// current_pml4 (which always stays as the kernel PML4).
//
// Returns: 1 = success, 0 = failure
// ============================================================
int vmm_map_page_into(uint64_t vaddr, uint64_t paddr, uint64_t flags,
                       phys_addr_t target_pml4_phys) {
    if (target_pml4_phys == PHYS_NULL) return 0;

    uint64_t* target_pml4 = (uint64_t*)PHYS_TO_VIRT(target_pml4_phys);
    uint64_t pml4_idx = PML4_IDX(vaddr);
    uint64_t pdpt_idx = PDPT_IDX(vaddr);
    uint64_t pd_idx   = PD_IDX(vaddr);
    uint64_t pt_idx   = PT_IDX(vaddr);

    uint64_t irq = spinlock_lock_irqsave(&paging_lock);

    // Level 4 (PML4) → Level 3 (PDPT)
    if (!(target_pml4[pml4_idx] & 1)) {
        uint64_t* new_pdpt = alloc_page_virt();
        if (!new_pdpt) {
            spinlock_unlock_irqrestore(&paging_lock, irq);
            return 0;
        }
        target_pml4[pml4_idx] = VIRT_TO_PHYS(new_pdpt) | 7;
    }
    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(target_pml4[pml4_idx] & PAGE_MASK);

    // Level 3 (PDPT) → Level 2 (PD)
    if (pdpt[pdpt_idx] & (1ULL << 7)) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return 0;
    }
    if (!(pdpt[pdpt_idx] & 1)) {
        uint64_t* new_pd = alloc_page_virt();
        if (!new_pd) {
            spinlock_unlock_irqrestore(&paging_lock, irq);
            return 0;
        }
        pdpt[pdpt_idx] = VIRT_TO_PHYS(new_pd) | 7;
    }
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdpt[pdpt_idx] & PAGE_MASK);

    // Level 2 (PD) → Level 1 (PT)
    if (pd[pd_idx] & (1ULL << 7)) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return 0;
    }
    if (!(pd[pd_idx] & 1)) {
        uint64_t* new_pt = alloc_page_virt();
        if (!new_pt) {
            spinlock_unlock_irqrestore(&paging_lock, irq);
            return 0;
        }
        pd[pd_idx] = VIRT_TO_PHYS(new_pt) | 7;
    }
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[pd_idx] & PAGE_MASK);

    // Install the mapping
    pt[pt_idx] = (paddr & PAGE_MASK) | flags;

    spinlock_unlock_irqrestore(&paging_lock, irq);
    return 1;
}

// ============================================================
// vmm_map_page — Map vaddr → paddr in current_pml4 (always kernel)
//
// Returns: 1 = success, 0 = failure (OOM or huge page collision)
// Caller MUST NOT hold paging_lock (we acquire it here).
// ============================================================
int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    if (!current_pml4) return 0;

    uint64_t pml4_idx = PML4_IDX(vaddr);
    uint64_t pdpt_idx = PDPT_IDX(vaddr);
    uint64_t pd_idx   = PD_IDX(vaddr);
    uint64_t pt_idx   = PT_IDX(vaddr);

    uint64_t irq = spinlock_lock_irqsave(&paging_lock);

    // Level 4 (PML4) → Level 3 (PDPT)
    if (!(current_pml4[pml4_idx] & 1)) {
        uint64_t* new_pdpt = alloc_page_virt();
        if (!new_pdpt) {
            spinlock_unlock_irqrestore(&paging_lock, irq);
            return 0;
        }
        current_pml4[pml4_idx] = VIRT_TO_PHYS(new_pdpt) | 7;
    }
    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(current_pml4[pml4_idx] & PAGE_MASK);

    // Level 3 (PDPT) → Level 2 (PD)
    if (pdpt[pdpt_idx] & (1ULL << 7)) {
        // 1GB huge page — cannot split
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return 0;
    }
    if (!(pdpt[pdpt_idx] & 1)) {
        uint64_t* new_pd = alloc_page_virt();
        if (!new_pd) {
            spinlock_unlock_irqrestore(&paging_lock, irq);
            return 0;
        }
        pdpt[pdpt_idx] = VIRT_TO_PHYS(new_pd) | 7;
    }
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdpt[pdpt_idx] & PAGE_MASK);

    // Level 2 (PD) → Level 1 (PT)
    if (pd[pd_idx] & (1ULL << 7)) {
        // 2MB huge page — cannot split
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return 0;
    }
    if (!(pd[pd_idx] & 1)) {
        uint64_t* new_pt = alloc_page_virt();
        if (!new_pt) {
            spinlock_unlock_irqrestore(&paging_lock, irq);
            return 0;
        }
        pd[pd_idx] = VIRT_TO_PHYS(new_pt) | 7;
    }
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[pd_idx] & PAGE_MASK);

    // Install the mapping
    pt[pt_idx] = (paddr & PAGE_MASK) | flags;

    spinlock_unlock_irqrestore(&paging_lock, irq);

    vmm_tlb_shootdown(vaddr);
    return 1;
}

// ============================================================
// vmm_unmap_page — Remove mapping for vaddr
//
// Returns: 1 if unmapped, 0 if not mapped
// ============================================================
int vmm_unmap_page(uint64_t vaddr) {
    if (!current_pml4) return 0;

    uint64_t pml4_idx = PML4_IDX(vaddr);
    uint64_t pdpt_idx = PDPT_IDX(vaddr);
    uint64_t pd_idx   = PD_IDX(vaddr);
    uint64_t pt_idx   = PT_IDX(vaddr);

    uint64_t irq = spinlock_lock_irqsave(&paging_lock);

    if (!(current_pml4[pml4_idx] & 1)) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return 0;
    }
    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(current_pml4[pml4_idx] & PAGE_MASK);

    if (!(pdpt[pdpt_idx] & 1)) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return 0;
    }
    if (pdpt[pdpt_idx] & (1ULL << 7)) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return 0; // huge page
    }
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdpt[pdpt_idx] & PAGE_MASK);

    if (!(pd[pd_idx] & 1)) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return 0;
    }
    if (pd[pd_idx] & (1ULL << 7)) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return 0; // huge page
    }
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[pd_idx] & PAGE_MASK);

    if (!(pt[pt_idx] & 1)) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return 0;
    }

    pt[pt_idx] = 0;

    spinlock_unlock_irqrestore(&paging_lock, irq);

    vmm_tlb_shootdown(vaddr);
    return 1;
}

// ============================================================
// vmm_unmap_page_from — Unmap vaddr from a SPECIFIC PML4
//
// FIX_005 Tahap 3: dipakai uheap untuk melepas region user dari AS
// task tanpa menghancurkan seluruh AS. Mengembalikan physical address
// halaman yang di-unmap (PHYS_NULL jika tidak mapped / huge page)
// supaya caller bisa mengembalikan frame ke PMM.
//
// target_pml4_phys == PHYS_NULL → kernel PML4 (current_pml4).
// invlpg hanya CPU ini — batasan yang sama dengan vmm_tlb_shootdown.
// ============================================================
phys_addr_t vmm_unmap_page_from(uint64_t vaddr, phys_addr_t target_pml4_phys) {
    uint64_t* pml4 = (target_pml4_phys != PHYS_NULL)
                   ? (uint64_t*)PHYS_TO_VIRT(target_pml4_phys)
                   : current_pml4;
    if (!pml4) return PHYS_NULL;

    uint64_t irq = spinlock_lock_irqsave(&paging_lock);

    if (!(pml4[PML4_IDX(vaddr)] & 1)) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return PHYS_NULL;
    }
    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(pml4[PML4_IDX(vaddr)] & PAGE_MASK);

    if (!(pdpt[PDPT_IDX(vaddr)] & 1) || (pdpt[PDPT_IDX(vaddr)] & (1ULL << 7))) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return PHYS_NULL;
    }
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdpt[PDPT_IDX(vaddr)] & PAGE_MASK);

    if (!(pd[PD_IDX(vaddr)] & 1) || (pd[PD_IDX(vaddr)] & (1ULL << 7))) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return PHYS_NULL;
    }
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PAGE_MASK);

    if (!(pt[PT_IDX(vaddr)] & 1)) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return PHYS_NULL;
    }

    phys_addr_t page_phys = (phys_addr_t)(pt[PT_IDX(vaddr)] & PAGE_MASK);
    pt[PT_IDX(vaddr)] = 0;

    spinlock_unlock_irqrestore(&paging_lock, irq);

    vmm_tlb_shootdown(vaddr);
    return page_phys;
}

// ============================================================
// vmm_alloc_page_into — Allocate physical page + map into a SPECIFIC PML4
//
// target_pml4 == PHYS_NULL → map into the kernel PML4 (vmm_map_page).
// FIX_002: target PML4 selalu eksplisit dari pemanggil — tidak ada lagi
// routing lewat global (race ELF load dua CPU lewat vmm_user_pml4).
// ============================================================
int vmm_alloc_page_into(uint64_t vaddr, uint64_t flags, phys_addr_t target_pml4) {
    phys_addr_t paddr = pmm_alloc_page();
    if (paddr == PHYS_NULL) return 0;

    int ok = (target_pml4 != PHYS_NULL)
           ? vmm_map_page_into(vaddr, paddr, flags, target_pml4)
           : vmm_map_page(vaddr, paddr, flags);

    if (!ok) {
        pmm_free_page(paddr);
        return 0;
    }
    return 1;
}

// ============================================================
// vmm_alloc_page — Allocate physical page + map into KERNEL PML4
// ============================================================
int vmm_alloc_page(uint64_t vaddr, uint64_t flags) {
    return vmm_alloc_page_into(vaddr, flags, PHYS_NULL);
}

int paging_map_region(uint64_t vaddr) {
    return vmm_alloc_page(vaddr, 7);
}

// ============================================================
// paging_is_mapped_into — Check if vaddr is mapped in a SPECIFIC PML4
//
// pml4_phys == PHYS_NULL → kernel PML4 (current_pml4). SMP-safe.
// ============================================================
// Penelusuran tabel MURNI (tanpa lock). Dipakai oleh paging_is_mapped_into()
// (dengan lock) dan paging_is_mapped_nolock() (jalur panic, tanpa lock).
static int walk_is_mapped(uint64_t* check_pml4, uint64_t vaddr) {
    if (!check_pml4) return 0;

    uint64_t pml4_idx = PML4_IDX(vaddr);
    if (!(check_pml4[pml4_idx] & 1)) return 0;
    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(check_pml4[pml4_idx] & PAGE_MASK);

    uint64_t pdpt_idx = PDPT_IDX(vaddr);
    if (!(pdpt[pdpt_idx] & 1)) return 0;
    if (pdpt[pdpt_idx] & (1ULL << 7)) return 1;   // 1GB huge page "mapped"
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdpt[pdpt_idx] & PAGE_MASK);

    uint64_t pd_idx = PD_IDX(vaddr);
    if (!(pd[pd_idx] & 1)) return 0;
    if (pd[pd_idx] & (1ULL << 7)) return 1;       // 2MB huge page "mapped"
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[pd_idx] & PAGE_MASK);

    uint64_t pt_idx = PT_IDX(vaddr);
    return (pt[pt_idx] & 1) ? 1 : 0;
}

int paging_is_mapped_into(uint64_t vaddr, phys_addr_t pml4_phys) {
    uint64_t* check_pml4 = (pml4_phys != PHYS_NULL)
                         ? (uint64_t*)PHYS_TO_VIRT(pml4_phys)
                         : current_pml4;
    if (!check_pml4) return 0;

    uint64_t irq = spinlock_lock_irqsave(&paging_lock);
    int result = walk_is_mapped(check_pml4, vaddr);
    spinlock_unlock_irqrestore(&paging_lock, irq);
    return result;
}

// ============================================================
// paging_is_mapped — Check vaddr in the KERNEL PML4 (SMP-safe)
// ============================================================
int paging_is_mapped(uint64_t vaddr) {
    return paging_is_mapped_into(vaddr, PHYS_NULL);
}

// ============================================================
// paging_is_mapped_nolock — HANYA UNTUK JALUR PANIC
//
// Sama seperti paging_is_mapped() tetapi TIDAK mengambil paging_lock.
//
// Kenapa wajib: kalau fault terjadi di CPU yang SEDANG memegang paging_lock
// (mis. fault saat kernel memetakan halaman untuk aplikasi), maka handler
// panic yang memakai versi ber-lock akan menunggu lock itu SELAMANYA
// (self-deadlock) — gejalanya: sistem membeku, layar BSOD TIDAK PERNAH
// muncul, dan tidak ada reboot. Pembacaan tabel tanpa lock aman di sini:
// tiap entri dibaca sebagai 8 byte atomic dan hasilnya hanya dipakai untuk
// diagnosa, bukan untuk mengubah pemetaan.
// ============================================================
int paging_is_mapped_nolock(uint64_t vaddr) {
    return walk_is_mapped(current_pml4, vaddr);
}

// ============================================================
// vmm_create_address_space — Create new PML4 for a process
//
// Allocates a fresh PML4 page and clones kernel mappings
// (PML4 entries 256..511) from the current (boot) PML4.
//
// Returns: physical address of new PML4, or PHYS_NULL on failure.
// ============================================================
phys_addr_t vmm_create_address_space(void) {
    if (!current_pml4) return PHYS_NULL;

    uint64_t irq = spinlock_lock_irqsave(&paging_lock);

    // Allocate new PML4
    uint64_t* new_pml4 = alloc_page_virt();
    if (!new_pml4) {
        spinlock_unlock_irqrestore(&paging_lock, irq);
        return PHYS_NULL;
    }

    // Clone kernel mappings: PML4[256..511] (higher-half)
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = current_pml4[i];
    }

    // User entries [0..255] are already zero (alloc_page_virt zeroes)
    phys_addr_t new_pml4_phys = VIRT_TO_PHYS(new_pml4);

    spinlock_unlock_irqrestore(&paging_lock, irq);
    return new_pml4_phys;
}

// ============================================================
// vmm_clone_user_as — Full-copy fork of a user address space (P0 Phase 6B).
//
// Strategy: FULL PHYSICAL COPY (not COW). COW was rejected: no page
// refcounts exist, #PF always panics (no write-fault hook), and TLB
// shootdown is local-only (vmm_tlb_shootdown) — remote COW invalidation
// is unsolved debt. Full-copy needs none of that.
//
// Clones PML4[0..255] (user half) page by page: fresh PML4 via
// vmm_create_address_space (kernel halves [256..511] shared by value,
// never deep-copied), then for every present 4KB PTE: fresh frame +
// 4KB memcpy via HHDM (supervisor mapping — no SMAP window needed) +
// map with IDENTICAL flag bits. Huge/empty entries are skipped exactly
// like vmm_destroy_address_space (no huge pages exist in user range:
// ELF/uheap map 4KB only).
//
// Parent tables are only READ (never modified: no parent TLB work).
// Child tables were never loaded: no invalidation needed.
// The caller (parent itself, inside its fork syscall) owns its user
// mappings exclusively — no other CPU touches them, no demand paging
// exists — so the unlocked walk below cannot race.
//
// Failure: partial child AS destroyed via vmm_destroy_address_space
// (frees child frames + tables walked so far), PHYS_NULL returned.
// The parent is never modified on any path.
// ============================================================
phys_addr_t vmm_clone_user_as(phys_addr_t parent_pml4_phys) {
    if (parent_pml4_phys == PHYS_NULL || !current_pml4) return PHYS_NULL;

    phys_addr_t child_pml4_phys = vmm_create_address_space();
    if (child_pml4_phys == PHYS_NULL) return PHYS_NULL;

    uint64_t* ppml4 = (uint64_t*)PHYS_TO_VIRT(parent_pml4_phys);

    for (uint64_t p4 = 0; p4 < 256; p4++) {
        uint64_t e4 = ppml4[p4];
        if (!(e4 & 1) || (e4 & (1ULL << 7))) continue;   // absent/huge: skip
        uint64_t* ppdpt = (uint64_t*)PHYS_TO_VIRT(e4 & PAGE_MASK);

        for (uint64_t p3 = 0; p3 < 512; p3++) {
            uint64_t e3 = ppdpt[p3];
            if (!(e3 & 1) || (e3 & (1ULL << 7))) continue;
            uint64_t* ppd = (uint64_t*)PHYS_TO_VIRT(e3 & PAGE_MASK);

            for (uint64_t p2 = 0; p2 < 512; p2++) {
                uint64_t e2 = ppd[p2];
                if (!(e2 & 1) || (e2 & (1ULL << 7))) continue;
                uint64_t* ppt = (uint64_t*)PHYS_TO_VIRT(e2 & PAGE_MASK);

                for (uint64_t p1 = 0; p1 < 512; p1++) {
                    uint64_t e1 = ppt[p1];
                    if (!(e1 & 1)) continue;
                    uint64_t vaddr = (p4 << 39) | (p3 << 30) | (p2 << 21) | (p1 << 12);
                    phys_addr_t src = (phys_addr_t)(e1 & PAGE_MASK);
                    uint64_t flags = e1 & ~PAGE_MASK;   // all flag bits incl. NX

                    phys_addr_t dst = pmm_alloc_page();
                    if (dst == PHYS_NULL) goto oom;
                    memcpy((void*)PHYS_TO_VIRT(dst), (const void*)PHYS_TO_VIRT(src), 4096);
                    if (!vmm_map_page_into(vaddr, dst, flags, child_pml4_phys)) {
                        pmm_free_page(dst);
                        goto oom;
                    }
                }
            }
        }
    }
    return child_pml4_phys;

oom:
    vmm_destroy_address_space(child_pml4_phys, 1);
    return PHYS_NULL;
}

// ============================================================
// vmm_destroy_address_space — Free entire page table hierarchy
//
// Walks PML4 entries 0..255 (user range) and frees:
//   - All 4KB data pages (PT entries)
//   - All PT pages
//   - All PD pages
//   - All PDPT pages
//   - Optionally the PML4 page itself
//
// Does NOT touch kernel mappings (entries 256..511).
// ============================================================
void vmm_destroy_address_space(phys_addr_t pml4_phys, int free_pml4) {
    if (pml4_phys == PHYS_NULL) return;

    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(pml4_phys);

    uint64_t irq = spinlock_lock_irqsave(&paging_lock);

    // Walk user-half PML4 entries: 0..255
    for (int p4 = 0; p4 < 256; p4++) {
        if (!(pml4[p4] & 1)) continue;

        phys_addr_t pdpt_phys = pml4[p4] & PAGE_MASK;
        uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(pdpt_phys);

        // Skip 1GB huge pages (shouldn't exist in user space, but guard)
        if (pml4[p4] & (1ULL << 7)) {
            pml4[p4] = 0;
            continue;
        }

        for (int p3 = 0; p3 < 512; p3++) {
            if (!(pdpt[p3] & 1)) continue;

            phys_addr_t pd_phys = pdpt[p3] & PAGE_MASK;

            // Skip 1GB huge pages
            if (pdpt[p3] & (1ULL << 7)) {
                pdpt[p3] = 0;
                continue;
            }

            uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pd_phys);

            for (int p2 = 0; p2 < 512; p2++) {
                if (!(pd[p2] & 1)) continue;

                phys_addr_t pt_phys = pd[p2] & PAGE_MASK;

                // Skip 2MB huge pages — just clear entry
                if (pd[p2] & (1ULL << 7)) {
                    pd[p2] = 0;
                    continue;
                }

                uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pt_phys);

                // Free every 4KB data page in this PT
                // Guard: only free pages that PMM actually allocated.
                // Limine boot mappings (identity maps, framebuffer) are NOT ours.
                for (int p1 = 0; p1 < 512; p1++) {
                    if (!(pt[p1] & 1)) continue;
                    phys_addr_t page_phys = pt[p1] & PAGE_MASK;
                    if (pmm_owns_page(page_phys)) pmm_free_page(page_phys);
                    pt[p1] = 0;
                }

                // Free the PT page itself
                if (pmm_owns_page(pt_phys)) pmm_free_page(pt_phys);
                pd[p2] = 0;
            }

            // Free the PD page itself
            if (pmm_owns_page(pd_phys)) pmm_free_page(pd_phys);
            pdpt[p3] = 0;
        }

        // Free the PDPT page itself
        if (pmm_owns_page(pdpt_phys)) pmm_free_page(pdpt_phys);
        pml4[p4] = 0;
    }

    spinlock_unlock_irqrestore(&paging_lock, irq);

    // Optionally free the PML4 page
    if (free_pml4) {
        if (pmm_owns_page(pml4_phys)) pmm_free_page(pml4_phys);
    }

    // Flush TLB since we just nuked mappings
    vmm_flush_tlb_all();
}

// ============================================================
// vmm_switch_pml4 — Switch CR3 to a given PML4 (for isolation)
//
// Does NOT change current_pml4 — it always stays as kernel PML4.
// Only CR3 is switched so the CPU uses the user PML4 for translations.
// ============================================================
void vmm_switch_pml4(phys_addr_t pml4_phys) {
    if (pml4_phys == PHYS_NULL) return;

    __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)pml4_phys) : "memory");

    uint32_t cpu_id = smp_current_cpu_index();
    percpu_t *cpu = smp_get_cpu(cpu_id);
    if (cpu) cpu->current_cr3 = (uint64_t)pml4_phys;
}

// ============================================================
// vmm_switch_to_kernel_as — Restore kernel CR3
// ============================================================
void vmm_switch_to_kernel_as(void) {
    if (kernel_pml4_phys == PHYS_NULL) return;

    __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)kernel_pml4_phys) : "memory");

    uint32_t cpu_id = smp_current_cpu_index();
    percpu_t *cpu = smp_get_cpu(cpu_id);
    if (cpu) cpu->current_cr3 = (uint64_t)kernel_pml4_phys;
}

// ============================================================
// vmm_get_kernel_pml4_phys — Return boot PML4 physical address
// ============================================================
phys_addr_t vmm_get_kernel_pml4_phys(void) {
    return kernel_pml4_phys;
}

// ============================================================
// vmm_destroy_task_as — Destroy a task's AS and restore kernel
//
// Safe to call even if current_pml4 points to the task's PML4.
// Flow: switch to kernel PML4 first → then destroy old PML4.
// ============================================================
void vmm_destroy_task_as(phys_addr_t pml4_phys) {
    if (pml4_phys == PHYS_NULL) return;

    // Switch to kernel PML4 BEFORE destroying (since current_pml4
    // might point into the PML4 we're about to free)
    vmm_switch_to_kernel_as();

    // Now safe to destroy the old address space
    vmm_destroy_address_space(pml4_phys, 1);
}

// ============================================================
// vmm_map_page_kernel — Map a page into the KERNEL PML4
//
// Used by expand_heap() to ensure kernel heap pages are always
// in the kernel PML4, regardless of which AS is currently active.
// ============================================================
int vmm_map_page_kernel(uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    return vmm_map_page_into(vaddr, paddr, flags, kernel_pml4_phys);
}

int vmm_alloc_page_kernel(uint64_t vaddr, uint64_t flags) {
    phys_addr_t paddr = pmm_alloc_page();
    if (paddr == PHYS_NULL) return 0;
    if (!vmm_map_page_kernel(vaddr, paddr, flags)) {
        pmm_free_page(paddr);
        return 0;
    }
#ifdef HEAP_WATCH_DEBUG
    // Jejak phys yang diberikan PMM untuk halaman heap — pembanding terhadap
    // PTE yang terbaca ulang saat watchpoint #DB menyala.
    extern void serial_print(const char* s);
    extern void serial_print_hex(uint64_t v);
    serial_print("[VMM] kmap va=");
    serial_print_hex(vaddr);
    serial_print(" pa=");
    serial_print_hex(paddr);
    serial_print("\n");
#endif
    return 1;
}
