#ifndef UHEAP_H
#define UHEAP_H

// FIX_005 Tahap 3 — heap user per proses.
//
// sys_alloc/sys_free/sys_realloc dari ring 3 tidak lagi memakai kmalloc:
// alokasi berupa region page-granular di USER RANGE milik AS pemanggil
// (model brk/region). Pointer yang lewat boundary bukan lagi alamat heap
// kernel — bug app tidak bisa menyentuh metadata kernel.
//
// KONTRAK:
//   - Semua fungsi HANYA dipanggil dari konteks syscall task pemilik
//     (state per-task, tidak ada lock — satu task = satu syscall aktif).
//   - uheap_realloc men-deref alamat user langsung untuk copy — CR3 saat
//     syscall = PML4 caller (int 0x80 tidak mengganti CR3).
//   - uheap_reset HANYA membebaskan metadata (node kernel heap) + reset brk.
//     Frame fisik region dibebaskan oleh vmm_destroy_address_space saat AS
//     dihancurkan — panggil uheap_reset berdampingan dengan teardown AS.

#include <stdint.h>
#include "task.h"

// Layout user range: ELF di 0x4000000, stack app [USER_STACK_TOP-256KB,
// USER_STACK_TOP), heap mulai 0x10000000 tumbuh ke atas.
#define UHEAP_BASE      0x10000000ULL
#define UHEAP_END       0x40000000ULL
// Plafon satu alokasi = UC_MAX_RANGE (canvas 4096x4096x4).
#define UHEAP_MAX_ALLOC (64ULL * 1024ULL * 1024ULL)

// Return: alamat user (>= UHEAP_BASE) atau 0 jika gagal (OOM / size 0 /
// task tanpa AS per-proses).
uint64_t uheap_alloc(task_t* t, uint64_t size);

// Validasi kepemilikan: hanya base region milik task ini yang diterima.
// Return 1 = region dibebaskan (unmap + frame ke PMM), 0 = pointer asing
// (diabaikan — tidak bisa merusak kernel).
int uheap_free(task_t* t, uint64_t uaddr);

// Semantik krealloc: uaddr 0 → alloc; new_size 0 → free (return 0);
// muat di kapasitas page region lama → in-place; selain itu alloc baru +
// copy + free lama. Return alamat baru atau 0 jika gagal (region lama utuh).
uint64_t uheap_realloc(task_t* t, uint64_t uaddr, uint64_t new_size);

// Bebaskan semua node metadata + brk kembali ke UHEAP_BASE.
void uheap_reset(task_t* t);

// Deep-copy heap metadata for fork() (P0 Phase 6B): duplicates the region
// node list into kernel-heap nodes owned by dst and copies brk. The region
// PAGES need no work — they live at the same user vaddrs, already cloned
// by vmm_clone_user_as. Returns 0 ok, -1 on node OOM (partial nodes freed
// via uheap_reset; caller destroys the child AS for the pages).
int uheap_clone(task_t* dst, const task_t* src);

#endif
