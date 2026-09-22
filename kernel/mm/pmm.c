// ============================================================
// kernel/mm/pmm.c — Physical Memory Manager (Bitmap Allocator)
//
// SMP-SAFE: All bitmap operations protected by pmm_lock.
// NO 4GB LIMIT: Bitmap is dynamically sized from Limine memmap.
//
// ARCHITECTURE:
//   - Bitmap: 1 bit per 4KB physical page (0 = free, 1 = used)
//   - Bitmap is allocated from first usable Limine region at boot
//   - Supports arbitrary RAM sizes (8GB, 16GB, 32GB, ...)
//   - Next-free hint avoids O(n) scan from page 0 every time
//   - used_pages counter for O(1) statistics
//   - Double-free and invalid-free detection
// ============================================================

#include "pmm.h"
#include "limine.h"
#include "spinlock.h"
#include <stddef.h>

// ============================================================
// State
// ============================================================

// Dynamic bitmap — allocated from Limine usable memory at boot
static uint8_t *pmm_bitmap    = NULL;
static uint64_t bitmap_bytes  = 0;
static uint64_t bitmap_pages  = 0;  // max page index the bitmap can track

// Total physical RAM from Limine
static uint64_t real_total_ram = 0;
static uint64_t total_pages    = 0;

// O(1) statistics
static uint64_t used_pages = 0;

// SMP lock
static spinlock_t pmm_lock = SPINLOCK_INIT;

// Next-free hint
static uint64_t next_free_hint = 0;

// ============================================================
// Bitmap helpers (caller MUST hold pmm_lock)
// ============================================================

static inline void bitmap_set(uint64_t bit) {
    pmm_bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7));
}

static inline void bitmap_clear_bit(uint64_t bit) {
    pmm_bitmap[bit >> 3] &= (uint8_t)~(1U << (bit & 7));
}

static inline int bitmap_test(uint64_t bit) {
    return (pmm_bitmap[bit >> 3] & (1U << (bit & 7))) != 0;
}

// ============================================================
// kprint helper (avoid include hell)
// ============================================================
extern void kprint(const char* str);
extern void kprint_num(uint64_t num);
extern uint64_t hhdm_offset;  // from kernel.c — HHDM virtual base

static void pmm_warn(const char* prefix, phys_addr_t addr) {
    kprint(prefix);
    kprint("0x");
    kprint_num(addr);
    kprint("\n");
}

// ============================================================
// pmm_init_dynamic — Parse Limine memmap, allocate bitmap
//
// Called ONCE during boot (single-threaded, no SMP yet).
//
// Phase 1: Scan memmap → find highest_addr
// Phase 2: Allocate bitmap from first large usable region
// Phase 3: Lock ALL pages, then free USABLE, then re-lock 72MB
// ============================================================
void pmm_init_dynamic(void* memmap_entries, uint64_t entry_count) {
    struct limine_memmap_entry **entries =
        (struct limine_memmap_entry **)memmap_entries;

    // ── Phase 1: Find highest usable address ──
    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < entry_count; i++) {
        struct limine_memmap_entry *e = entries[i];
        uint64_t end = e->base + e->length;
        if (e->type == LIMINE_MEMMAP_USABLE && end > highest_addr)
            highest_addr = end;
    }

    if (highest_addr == 0) {
        kprint("[PMM] PANIC: no usable memory reported!\n");
        for (;;) __asm__ volatile("hlt");
    }

    // ── Phase 2: Allocate bitmap from usable memory ──
    //
    // bitmap needs: (highest_addr / PAGE_SIZE) bits / 8 bytes
    // We over-allocate by 1 page for safety.
    uint64_t pages_needed  = highest_addr / PAGE_SIZE;
    uint64_t needed_bytes  = (pages_needed + 7) / 8;
    // Round up to whole pages
    uint64_t alloc_bytes   = (needed_bytes + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t alloc_pages   = alloc_bytes / PAGE_SIZE;

    // Find first usable region >= 72MB that fits the bitmap
    phys_addr_t bitmap_phys = 0;
    int found = 0;
    for (uint64_t i = 0; i < entry_count && !found; i++) {
        struct limine_memmap_entry *e = entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t base = e->base;
        uint64_t end  = base + e->length;

        // Page-align base up
        if (base & (PAGE_SIZE - 1))
            base = (base + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);

        // Skip first 72MB (kernel zone)
        if (base < 0x4800000ULL) base = 0x4800000ULL;
        if (base >= end) continue;

        if ((end - base) >= alloc_bytes) {
            bitmap_phys = base;
            found = 1;
        }
    }

    if (!found) {
        kprint("[PMM] PANIC: no usable region for bitmap!\n");
        for (;;) __asm__ volatile("hlt");
    }

    // Zero the bitmap — access via HHDM virtual address (NOT raw physical!)
    uint8_t *bmap = (uint8_t *)(bitmap_phys + hhdm_offset);
    for (uint64_t i = 0; i < alloc_bytes; i++) bmap[i] = 0;

    pmm_bitmap   = bmap;
    bitmap_bytes = alloc_bytes;
    bitmap_pages = pages_needed;

    // ── Phase 3: Build page ownership map ──

    // Step A: Lock ALL pages in bitmap (conservative default)
    for (uint64_t i = 0; i < alloc_bytes; i++) pmm_bitmap[i] = 0xFF;

    // Step B: Free only USABLE pages
    for (uint64_t i = 0; i < entry_count; i++) {
        struct limine_memmap_entry *e = entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t start = e->base;
        uint64_t end   = start + e->length;

        if (start & (PAGE_SIZE - 1))
            start = (start + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);

        for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
            uint64_t page = addr / PAGE_SIZE;
            if (page < bitmap_pages)
                bitmap_clear_bit(page);
        }
    }

    // Step C: Re-lock first 72MB (kernel, Limine, framebuffer)
    uint64_t kernel_lock_pages = 0x4800000ULL / PAGE_SIZE;
    for (uint64_t i = 0; i < kernel_lock_pages && i < bitmap_pages; i++)
        bitmap_set(i);

    // Step D: Lock bitmap pages themselves (they were freed in step B)
    uint64_t bmap_start_page = bitmap_phys / PAGE_SIZE;
    for (uint64_t i = 0; i < alloc_pages && (bmap_start_page + i) < bitmap_pages; i++)
        bitmap_set(bmap_start_page + i);

    // ── Phase 4: Set totals ──
    real_total_ram = highest_addr;
    total_pages    = highest_addr / PAGE_SIZE;
    if (total_pages > bitmap_pages) total_pages = bitmap_pages;

    // Count used pages for O(1) stats
    used_pages = 0;
    for (uint64_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i)) used_pages++;
    }

    next_free_hint = kernel_lock_pages;

    kprint("[PMM] RAM: ");
    kprint_num(real_total_ram / (1024 * 1024));
    kprint(" MB, bitmap: ");
    kprint_num(bitmap_bytes / 1024);
    kprint(" KB @ 0x");
    kprint_num(bitmap_phys);
    kprint("\n");
}

// ============================================================
// pmm_alloc_page — Allocate one 4KB physical page
//
// Two-phase scan: hint→end, then wrap 0→hint.
// Returns PHYS_NULL on OOM.
// ============================================================
phys_addr_t pmm_alloc_page(void) {
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);

    if (pmm_bitmap == NULL) {
        spinlock_unlock_irqrestore(&pmm_lock, flags);
        return PHYS_NULL;
    }

    uint64_t max = total_pages;

    // Phase 1: hint → end
    for (uint64_t i = next_free_hint; i < max; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
            next_free_hint = i + 1;
            phys_addr_t result = i * PAGE_SIZE;
            spinlock_unlock_irqrestore(&pmm_lock, flags);
            return result;
        }
    }

    // Phase 2: wrap 0 → hint
    uint64_t wrap_end = (next_free_hint < max) ? next_free_hint : max;
    for (uint64_t i = 0; i < wrap_end; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
            next_free_hint = i + 1;
            phys_addr_t result = i * PAGE_SIZE;
            spinlock_unlock_irqrestore(&pmm_lock, flags);
            return result;
        }
    }

    // Out of physical memory
    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return PHYS_NULL;
}

// ============================================================
// pmm_free_page — Free one 4KB physical page
//
// Validates: alignment, range, double-free.
// Rewinds hint if freed page < hint.
// ============================================================
void pmm_free_page(phys_addr_t addr) {
    // ── P4: Invalid free detection ──
    if (addr == PHYS_NULL) {
        pmm_warn("[PMM] free NULL: ", addr);
        return;
    }

    if (addr & ((phys_addr_t)PAGE_SIZE - 1)) {
        pmm_warn("[PMM] free unaligned: ", addr);
        return;
    }

    uint64_t bit = addr / PAGE_SIZE;

    if (pmm_bitmap == NULL || bit >= bitmap_pages) {
        pmm_warn("[PMM] free out-of-range: ", addr);
        return;
    }

    // Zona < 72MB (kernel, Limine, BIOS/ROM, framebuffer) tidak pernah
    // dialokasikan PMM — free di zona ini SELALU bug pemanggil. Menolaknya
    // menjaga free list tidak tercemar halaman reserved/ROM (akar BOSD heap).
    if (addr < 0x4800000ULL) {
        pmm_warn("[PMM] free low-reserved (BUG, ditolak): ", addr);
        return;
    }

    uint64_t irqflags = spinlock_lock_irqsave(&pmm_lock);

    // ── P3: Double-free detection ──
    if (!bitmap_test(bit)) {
        spinlock_unlock_irqrestore(&pmm_lock, irqflags);
        pmm_warn("[PMM] double-free: ", addr);
        return;
    }

    bitmap_clear_bit(bit);
    if (used_pages > 0) used_pages--;

    // Rewind hint
    if (bit < next_free_hint)
        next_free_hint = bit;

    spinlock_unlock_irqrestore(&pmm_lock, irqflags);
}

// ============================================================
// pmm_owns_page — Check if PMM tracks and allocated this page
//
// Returns 1 if the address is within bitmap range AND marked used.
// Returns 0 if out-of-range or free (not ours to free).
// Lock-free: reads bitmap without lock (safe for diagnostic use).
// ============================================================
int pmm_owns_page(phys_addr_t addr) {
    if (addr == PHYS_NULL || (addr & ((phys_addr_t)PAGE_SIZE - 1))) return 0;
    if (pmm_bitmap == NULL) return 0;
    uint64_t bit = addr / PAGE_SIZE;
    if (bit >= bitmap_pages) return 0;
    return bitmap_test(bit) ? 1 : 0;
}

// ============================================================
// Statistics — O(1) via used_pages counter
// ============================================================

uint64_t pmm_get_used_pages(void) {
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);
    uint64_t val = used_pages;
    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return val;
}

uint64_t pmm_get_total_pages(void) {
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);
    uint64_t val = total_pages;
    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return val;
}

// ============================================================
// Getter TANPA LOCK — hanya untuk jalur panic/diagnosa.
//
// Alasannya sama dengan paging_is_mapped_nolock(): kalau fault terjadi di CPU
// yang sedang memegang pmm_lock (mis. fault di tengah pmm_alloc_page), handler
// panic yang memakai getter ber-lock akan menunggu selamanya → freeze tanpa
// BSOD. Kedua nilai adalah integer 64-bit yang dibaca atomic; dipakai hanya
// untuk menampilkan statistik, jadi tidak perlu konsisten sempurna.
// ============================================================
uint64_t pmm_get_used_pages_nolock(void)  { return used_pages; }
uint64_t pmm_get_total_pages_nolock(void) { return total_pages; }

uint64_t pmm_get_used_ram(void) {
    return pmm_get_used_pages() * PAGE_SIZE;
}

uint64_t pmm_get_total_ram(void) {
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);
    uint64_t val = real_total_ram;
    spinlock_unlock_irqrestore(&pmm_lock, flags);
    return val;
}

uint64_t pmm_get_free_ram(void) {
    uint64_t total = pmm_get_total_ram();
    uint64_t used  = pmm_get_used_ram();
    return (total > used) ? (total - used) : 0;
}

void pmm_set_total_ram(uint64_t size) {
    uint64_t flags = spinlock_lock_irqsave(&pmm_lock);
    real_total_ram = size;
    total_pages = size / PAGE_SIZE;
    if (total_pages > bitmap_pages) total_pages = bitmap_pages;
    spinlock_unlock_irqrestore(&pmm_lock, flags);
}
