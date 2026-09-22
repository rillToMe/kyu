#include "heap.h"
#include "string.h"
#include "paging.h"  // Untuk memanggil vmm_alloc_page()
#include "spinlock.h"
#include <stddef.h>  // size_t

#define HEAP_MAGIC 0xDEADC0DE

extern void kernel_panic(const char* title, const char* desc, uint64_t code);

#ifdef HEAP_WATCH_DEBUG
#define ALLOC_LOG_SIZE 64
extern void serial_print(const char* s);
extern void serial_print_hex(uint64_t val);
typedef struct {
    void*    ptr;
    size_t   size;
    void*    ra0;   // caller of kmalloc (often krealloc — see ra1/ra2 for real source)
    void*    ra1;
    void*    ra2;
} alloc_log_entry_t;
static alloc_log_entry_t alloc_log[ALLOC_LOG_SIZE];
static int alloc_log_idx = 0;

static void log_alloc(void* ptr, size_t size, void* ra0, void* ra1, void* ra2) {
    alloc_log[alloc_log_idx].ptr  = ptr;
    alloc_log[alloc_log_idx].size = size;
    alloc_log[alloc_log_idx].ra0  = ra0;
    alloc_log[alloc_log_idx].ra1  = ra1;
    alloc_log[alloc_log_idx].ra2  = ra2;
    alloc_log_idx = (alloc_log_idx + 1) % ALLOC_LOG_SIZE;
}

// key=value pair on one line (label + hex), no newline
static void log_kv(const char* k, uint64_t v) {
    serial_print(k); serial_print("0x"); serial_print_hex(v);
}
#endif

// Zona Heap Virtual — HARUS canonical (bit47=1, bits63-48=0xFFFF)
// dan di luar HHDM Limine (0xFFFF800000000000 + RAM size, biasanya < 0xFFFF810000000000)
//
// 0xFFFF000000000000 → NON-CANONICAL (bit47=0, bits63-48=0xFFFF) → GPF!
// 0xFFFF900000000000 → CANONICAL (bit47=1) + jauh dari HHDM dan kernel (PML4[288])
#define HEAP_START_VADDR 0xFFFF900000000000ULL

static uint64_t current_heap_end = HEAP_START_VADDR;

heap_block_t* heap_head = NULL;
static spinlock_t heap_lock = SPINLOCK_INIT;

#ifdef HEAP_WATCH_DEBUG
// True only if `p` is canonical AND the page is mapped — safe to dereference.
// The corrupt heap already holds garbage next-pointers; walking them blindly
// GPFs before the useful alloc history can print. Guard every deref with this.
static int hb_readable(const void* p) {
    uint64_t v = (uint64_t)p;
    uint64_t top = v >> 47;               // canonical: 0 or 0x1FFFF
    if (top != 0 && top != 0x1FFFF) return 0;
    return paging_is_mapped(v) && paging_is_mapped(v + sizeof(heap_block_t) - 1);
}

// Full dump on corruption: bad block metadata, expected vs actual canary,
// neighbour blocks, then the last 20 allocations (ptr/size/ra0/ra1/ra2).
static void dump_corruption(heap_block_t* bad) {
    serial_print("\n\n=== HEAP CORRUPTION ===\n");
    log_kv("block   = ", (uint64_t)bad); serial_print("\n");
    if (hb_readable(bad)) {
        log_kv("  size  = ", (uint64_t)bad->size); serial_print("\n");
        log_kv("  free  = ", (uint64_t)bad->is_free); serial_print("\n");
        log_kv("  next  = ", (uint64_t)bad->next); serial_print("\n");
        log_kv("canary expected = ", (uint64_t)HEAP_MAGIC); serial_print("\n");
        log_kv("canary actual   = ", (uint64_t)bad->magic); serial_print("\n");
    } else {
        serial_print("  (block ptr not readable — skipping field dump)\n");
    }

    // Print alloc history FIRST — this is the most useful data and must
    // survive even when the linked-list walk below faults on garbage pointers.
    serial_print("\n--- riwayat 20 alokasi terakhir ---\n");
    for (int k = 0; k < 20; k++) {
        int idx = (alloc_log_idx - 1 - k + ALLOC_LOG_SIZE) % ALLOC_LOG_SIZE;
        log_kv("  ptr=",   (uint64_t)alloc_log[idx].ptr);
        log_kv(" size=",   (uint64_t)alloc_log[idx].size);
        log_kv(" ra0=",    (uint64_t)alloc_log[idx].ra0);
        log_kv(" ra1=",    (uint64_t)alloc_log[idx].ra1);
        log_kv(" ra2=",    (uint64_t)alloc_log[idx].ra2);
        serial_print("\n");
    }
    serial_print("=== Cari entri ptr+size PALING DEKAT ke 'block' di atas = buffer overflow. ");
    serial_print("ra0/ra1/ra2 lewat: nm build/myos.bin | sort ===\n\n");

    // Walk list to find the block physically before `bad` (prev in address order).
    // Guard every pointer before dereferencing — next-pointers may be garbage.
    heap_block_t* prev = NULL;
    for (heap_block_t* c = heap_head; c != NULL; ) {
        if (!hb_readable(c)) { serial_print("[walk stopped: unreadable ptr]\n"); break; }
        if (c != bad && (uint64_t)c < (uint64_t)bad &&
            (!prev || (uint64_t)c > (uint64_t)prev))
            prev = c;
        heap_block_t* nxt = c->next;
        if (c == bad) { c = nxt; continue; }
        c = nxt;
    }
    if (prev && hb_readable(prev)) {
        serial_print("prev block:\n");
        log_kv("  addr  = ", (uint64_t)prev); serial_print("\n");
        log_kv("  size  = ", (uint64_t)prev->size); serial_print("\n");
        log_kv("  magic = ", (uint64_t)prev->magic); serial_print("\n");
        log_kv("  end   = ", (uint64_t)((uint8_t*)prev + sizeof(heap_block_t) + prev->size));
        serial_print("  (overwrite past here hits the block above)\n");
    } else {
        serial_print("prev block: (none or unreadable)\n");
    }
    if (hb_readable(bad) && bad->next && hb_readable(bad->next)) {
        serial_print("next block:\n");
        log_kv("  addr  = ", (uint64_t)bad->next); serial_print("\n");
        log_kv("  magic = ", (uint64_t)bad->next->magic); serial_print("\n");
    }
}
#endif

// 1. Inisialisasi
void init_heap(void) {
    heap_head = NULL;
}

// ===================================================================
// FUNGSI INTI: MEMPERLUAS HEAP SECARA DINAMIS MENGGUNAKAN VMM!
// ===================================================================
static heap_block_t* expand_heap(size_t required_size) {
    size_t total_size    = required_size + sizeof(heap_block_t);
    uint64_t pages_needed = total_size / 4096;
    if (total_size % 4096 != 0) pages_needed++;

    uint64_t start_expansion_addr = current_heap_end;

    for (uint64_t i = 0; i < pages_needed; i++) {
        // FIX_005 Tahap 3: US=0 (flags 3, dulu 7) — heap kernel tidak lagi
        // terlihat ring 3; app yang menyentuh 0xFFFF9000... langsung #PF.
        if (!vmm_alloc_page_kernel(current_heap_end, 3)) {
            // Bug 3.1: RAM fisik habis di tengah ekspansi. Roll back halaman
            // yang sudah ter-map agar tidak bocor (unmap + free frame), dan
            // kembalikan current_heap_end ke posisi semula supaya ekspansi
            // berikutnya konsisten.
            uint64_t mapped = current_heap_end - start_expansion_addr;
            for (uint64_t j = 0; j < mapped; j += 4096) {
                phys_addr_t pa = vmm_unmap_page_from(
                    start_expansion_addr + j, vmm_get_kernel_pml4_phys());
                if (pa != PHYS_NULL) pmm_free_page(pa);
            }
            current_heap_end = start_expansion_addr;
            return NULL; // RAM fisik habis
        }
        current_heap_end += 4096;
    }

    // Jadikan halaman baru sebagai blok bebas
    heap_block_t* new_block = (heap_block_t*)start_expansion_addr;
    new_block->magic   = HEAP_MAGIC;
    new_block->size    = (size_t)(pages_needed * 4096) - sizeof(heap_block_t);
    new_block->is_free = 1;
    new_block->next    = NULL;

    if (heap_head == NULL) {
        heap_head = new_block;
    } else {
        heap_block_t* curr = heap_head;
        while (curr->next != NULL) curr = curr->next;
        curr->next = new_block;
    }

    return new_block;
}

// 2. Kmalloc: First-Fit + dynamic expand
void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 15) & ~(size_t)15;

    uint64_t flags = spinlock_lock_irqsave(&heap_lock);

    heap_block_t* current = heap_head;

    while (current != NULL) {
        if (current->magic != HEAP_MAGIC) {
#ifdef HEAP_WATCH_DEBUG
            dump_corruption(current);
#endif
            kernel_panic("HEAP CORRUPTION",
                         "heap_block_t magic mismatch - header corrupt",
                         (uint64_t)current);
        }
        if (current->is_free && current->size >= size) {
            #ifdef HEAP_WATCH_DEBUG
            serial_print("[BEFORE SPLIT]\n");
            log_kv("current=", (uint64_t)current); serial_print("\n");
            log_kv("current->size=", (uint64_t)current->size); serial_print("\n");
            log_kv("request=", (uint64_t)size); serial_print("\n");
            log_kv("header=", (uint64_t)sizeof(heap_block_t)); serial_print("\n");
            #endif
            // Splitting: potong jika blok terlalu besar
            if (current->size > size + sizeof(heap_block_t) + 1) {
                heap_block_t* new_block = (heap_block_t*)((uint8_t*)current + sizeof(heap_block_t) + size);
                new_block->magic   = HEAP_MAGIC;
                new_block->is_free = 1;
                new_block->size    = current->size - size - sizeof(heap_block_t);
                new_block->next    = current->next;

                current->next = new_block;
                current->size = size;

                #ifdef HEAP_WATCH_DEBUG
                serial_print("[AFTER SPLIT]\n");
                log_kv("new_block=", (uint64_t)new_block); serial_print("\n");
                log_kv("new_block->size=", (uint64_t)new_block->size); serial_print("\n");
                log_kv("new_block->magic=", (uint64_t)new_block->magic); serial_print("\n");
                log_kv("new_block->next=", (uint64_t)new_block->next); serial_print("\n");
                #endif
#ifdef HEAP_WATCH_DEBUG
                serial_print("[heap] SPLIT ");
                log_kv("block=", (uint64_t)current);
                log_kv(" alloc=", (uint64_t)size);
                log_kv(" remainder=", (uint64_t)new_block);
                log_kv(" rem_size=", (uint64_t)new_block->size);
                serial_print("\n");
#endif
            }
            current->is_free = 0;
            void* result = (void*)((uint8_t*)current + sizeof(heap_block_t));
            spinlock_unlock_irqrestore(&heap_lock, flags);
#ifdef HEAP_WATCH_DEBUG
            log_alloc(result, size, __builtin_return_address(0),
                      __builtin_return_address(1), __builtin_return_address(2));
            serial_print("[heap] KMALLOC ");
            log_kv("ptr=", (uint64_t)result);
            log_kv(" size=", (uint64_t)size);
            log_kv(" ra0=", (uint64_t)__builtin_return_address(0));
            log_kv(" ra1=", (uint64_t)__builtin_return_address(1));
            log_kv(" ra2=", (uint64_t)__builtin_return_address(2));
            serial_print("\n");
#endif
            return result;
        }
        current = current->next;
    }

    // Tidak ada blok cocok — ekspansi heap
    heap_block_t* fresh_block = expand_heap(size);
    if (fresh_block == NULL) {
        spinlock_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }
    spinlock_unlock_irqrestore(&heap_lock, flags);
    return kmalloc(size);
}

// 3. Kfree: Bebaskan + Coalesce
void kfree(void* ptr) {
    if (ptr == NULL) return;

    uint64_t flags = spinlock_lock_irqsave(&heap_lock);

    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    if (block->magic != HEAP_MAGIC) {
#ifdef HEAP_WATCH_DEBUG
        dump_corruption(block);
#endif
        kernel_panic("HEAP CORRUPTION",
                     "heap_block_t magic mismatch - header corrupt",
                     (uint64_t)block);
    }
#ifdef HEAP_WATCH_DEBUG
    serial_print("[heap] KFREE ");
    log_kv("ptr=", (uint64_t)ptr);
    log_kv(" size=", (uint64_t)block->size);
    log_kv(" ra0=", (uint64_t)__builtin_return_address(0));
    serial_print("\n");
#endif
    block->is_free = 1;

    // Coalescing: lebur blok kosong yang bersebelahan
    heap_block_t* current = heap_head;
    while (current != NULL) {
        if (current->magic != HEAP_MAGIC) {
#ifdef HEAP_WATCH_DEBUG
            dump_corruption(current);
#endif
            kernel_panic("HEAP CORRUPTION",
                         "heap_block_t magic mismatch - header corrupt",
                         (uint64_t)current);
        }
        heap_block_t* expected_next = (heap_block_t*)((uint8_t*)current + sizeof(heap_block_t) + current->size);
        if (current->is_free && current->next != NULL && current->next->is_free &&
            current->next == expected_next) {
            #ifdef HEAP_WATCH_DEBUG
serial_print("[MERGE CHECK]\n");
log_kv("current=", (uint64_t)current); serial_print("\n");
log_kv("expected_next=", (uint64_t)expected_next); serial_print("\n");
log_kv("actual_next=", (uint64_t)current->next); serial_print("\n");
#endif

#ifdef HEAP_WATCH_DEBUG
            serial_print("[heap] MERGE ");
            log_kv("into=", (uint64_t)current);
            log_kv(" absorb=", (uint64_t)current->next);
            log_kv(" new_size=", (uint64_t)(current->size + current->next->size + sizeof(heap_block_t)));
            serial_print("\n");
#endif
            current->size += current->next->size + sizeof(heap_block_t);
            current->next  = current->next->next;
        } else {
            current = current->next;
        }
    }

    spinlock_unlock_irqrestore(&heap_lock, flags);
}

// 4. Krealloc: Ubah ukuran
void* krealloc(void* ptr, size_t old_size, size_t new_size) {
    if (new_size == 0) { kfree(ptr); return NULL; }
    if (ptr == NULL)   return kmalloc(new_size);

    void* new_ptr = kmalloc(new_size);
    if (new_ptr == NULL) return NULL;

    size_t copied = (old_size < new_size) ? old_size : new_size;
    memcpy(new_ptr, ptr, copied);
    kfree(ptr);
#ifdef HEAP_WATCH_DEBUG
    serial_print("[heap] KREALLOC ");
    log_kv("old_ptr=", (uint64_t)ptr);
    log_kv(" old_size=", (uint64_t)old_size);
    log_kv(" new_size=", (uint64_t)new_size);
    log_kv(" new_ptr=", (uint64_t)new_ptr);
    serial_print((new_ptr == ptr) ? " REUSE" : " MOVED");
    log_kv(" copied=", (uint64_t)copied);
    log_kv(" ra0=", (uint64_t)__builtin_return_address(0));
    serial_print("\n");
#endif
    return new_ptr;
}