// ============================================================
// kernel/sched/task_stack.c — Alokator stack task dengan guard page (ARCH-2b)
//
// MASALAH YANG DITUTUP
//   Stack task kernel dialokasikan dengan `kmalloc(TASK_STACK_SIZE)`. Heap
//   kernel tidak punya guard page: overflow stack yang tumbuh ke BAWAH akan
//   diam-diam menimpa blok heap berikutnya (korupsi data senyap), lalu baru
//   terdeteksi jauh kemudian saat `kfree` menemukan magic rusak — atau tidak
//   terdeteksi sama sekali.
//
//   Setelah ARCH-2a (#DF/NMI/#MC memakai IST) konsekuensinya sudah jauh lebih
//   baik: overflow → #PF di guard → #DF di IST stack → BSOD yang bisa dibaca,
//   bukan triple fault. Tapi itu masih "crash yang bisa dibaca". Guard page
//   membuat overflow terdeteksi PADA INSTRUKSI PERTAMA yang menyentuhnya,
//   sebagai #PF dengan CR2 tepat di halaman guard — bukan korupsi heap.
//
// KENAPA BUKAN kmalloc + unmap
//   Tempting: alokasikan TASK_STACK_SIZE + PAGE lewat kmalloc, lalu unmap
//   halaman terbawah. Itu SALAH — halaman itu tetap milik blok heap, dan
//   `kfree` nanti akan menulis header blok di sana → #PF di jalur free, atau
//   lebih buruk: heap allocator membaca header yang hilang saat walk list.
//
//   Jadi stack task dialokasikan dari PMM dan di-map manual, TERPISAH dari
//   heap. Ini pola yang sama dengan Linux (`alloc_thread_stack_node`): stack
//   kernel bukan objek heap.
//
// LAYOUT
//
//     +------------------------+  <- base + TASK_STACK_SIZE + PAGE
//     |  halaman stack (top)   |     (stack tumbuh ke bawah dari sini)
//     |         ...            |
//     |  halaman stack (bot)   |
//     +------------------------+  <- base + PAGE      <-- guard berakhir
//     |   GUARD (tidak mapped) |     #PF + CR2 = base
//     +------------------------+  <- base
//
//   Halaman guard TIDAK di-map sama sekali (bukan "read-only"): akses apa pun
//   — baca, tulis, eksekusi — menghasilkan #PF. Itu yang kita mau: tidak ada
//   akses sah ke guard, jadi tidak ada alasan membuatnya readable.
// ============================================================

#include <stdint.h>
#include <stddef.h>   // NULL
#include "task.h"
#include "paging.h"
#include "pmm.h"
#include "smp.h"
#include "kprint.h"

// PAGE_SIZE sudah didefinisikan di include/pmm.h (yang di-include di atas).
// JANGAN mendefinisikannya ulang di sini — clang memberi warning
// `-Wmacro-redefined` dan nilainya bisa berbeda tanpa terlihat.

// Alamat virtual awal area stack task.
//
// Dipilih di PML4[287] — satu slot di bawah zona heap kernel
// (0xFFFF900000000000 = PML4[288]), dan JAUH dari kernel image
// (0xFFFFFFFF80000000 = PML4[511]) maupun HHDM. Dengan begitu stack task
// tidak akan pernah bentrok dengan heap, kernel, atau mapping bootloader.
#define KZ_TASK_STACK_VADDR_BASE 0xFFFF880000000000ULL

// Berapa banyak stack yang bisa dialokasikan bersamaan. MAX_TASKS=16, tapi
// idle + syscall stack per-CPU juga dialokasikan dari sini, jadi beri ruang
// lebih. Tidak masalah kalau tidak terpakai — hanya alamat, bukan memori.
#define KZ_TASK_STACK_SLOTS 128

static uint64_t next_stack_vaddr = KZ_TASK_STACK_VADDR_BASE;
static uint32_t stacks_in_use = 0;

// Ukuran total satu slot: guard (1 page) + stack.
static uint64_t stack_slot_bytes(void) {
    return TASK_STACK_SIZE + PAGE_SIZE;
}

// ------------------------------------------------------------
// task_stack_alloc — alokasi stack dengan guard page di bawahnya.
//
// Return: alamat BASE area stack (menunjuk ke halaman GUARD), atau NULL bila
// gagal. Pemanggil menghitung `stack_top = base + PAGE_SIZE + TASK_STACK_SIZE`.
//
// PENTING: nilai yang disimpan di TCB (`stack_base`) harus BASE ini, karena
// task_stack_free() membutuhkannya untuk meng-unmap kedua bagian.
// ------------------------------------------------------------
void* task_stack_alloc(void) {
    if (stacks_in_use >= KZ_TASK_STACK_SLOTS) {
        kprint("[stack] kehabisan slot stack task\n");
        return NULL;
    }

    uint64_t base = next_stack_vaddr;
    uint64_t guard = base;
    uint64_t stack_lo = base + PAGE_SIZE;

    // 1. Map halaman stack (US=0 — kernel-only, RW).
    //    Loop supaya kegagalan di tengah bisa di-rollback bersih: tidak
    //    meninggalkan frame yang sudah ter-map tapi tidak terlacak.
    uint64_t mapped = 0;
    for (uint64_t va = stack_lo; va < stack_lo + TASK_STACK_SIZE; va += PAGE_SIZE) {
        if (!vmm_alloc_page_kernel(va, PAGING_PRESENT | PAGING_WRITABLE)) {
            // Rollback: lepas halaman yang sudah ter-map dan kembalikan
            // frame-nya ke PMM.
            for (uint64_t rb = stack_lo; rb < stack_lo + mapped; rb += PAGE_SIZE) {
                phys_addr_t pa = vmm_unmap_page_from(rb, PHYS_NULL);
                if (pa != PHYS_NULL) pmm_free_page(pa);
            }
            kprint("[stack] OOM memetakan stack task\n");
            return NULL;
        }
        mapped += PAGE_SIZE;
    }

    // 2. Guard page SENGAJA TIDAK di-map. Halaman ini tidak pernah dialokasi
    //    dari PMM, jadi tidak ada frame yang bocor karena "tidak di-map".
    //    #PF yang menyentuhnya akan melaporkan CR2 == guard.
    (void)guard;

    next_stack_vaddr += stack_slot_bytes();
    stacks_in_use++;
    return (void*)base;
}

// ------------------------------------------------------------
// task_stack_free — lepas stack + kembalikan frame ke PMM.
//
// `base` = nilai yang dikembalikan task_stack_alloc (BUKAN stack_top).
//
// Idempotent terhadap NULL. Tidak mengembalikan slot ke freelist (slot
// dialokasikan monotonik — 128 slot × 20 KB = 2.5 MB ruang alamat, dan
// MAX_TASKS=16 berarti slot tidak akan habis dalam praktik).
// ------------------------------------------------------------
void task_stack_free(void* base) {
    if (base == NULL) return;

    uint64_t b = (uint64_t)base;
    uint64_t stack_lo = b + PAGE_SIZE;

    for (uint64_t va = stack_lo; va < stack_lo + TASK_STACK_SIZE; va += PAGE_SIZE) {
        phys_addr_t pa = vmm_unmap_page_from(va, PHYS_NULL);
        if (pa != PHYS_NULL) pmm_free_page(pa);
    }

    if (stacks_in_use > 0) stacks_in_use--;
}

// ------------------------------------------------------------
// task_stack_top — ujung atas stack (tempat frame dibangun).
// ------------------------------------------------------------
uint64_t task_stack_top(void* base) {
    if (base == NULL) return 0;
    return (uint64_t)base + PAGE_SIZE + TASK_STACK_SIZE;
}

// ------------------------------------------------------------
// task_stack_guard_addr — alamat halaman guard (untuk diagnostik/panic).
// ------------------------------------------------------------
uint64_t task_stack_guard_addr(void* base) {
    return (uint64_t)base;
}
