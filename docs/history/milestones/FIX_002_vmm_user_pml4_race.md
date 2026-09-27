# FIX 002 — Global mutable `vmm_user_pml4` (race ELF load di SMP)

> **Prioritas**: P1
> **Status**: **DONE** (2026-07-26) — `vmm_user_pml4` dihapus total; regresi runtime bersih di `-smp 4`
> **Lokasi**: `kernel/paging.c` (`vmm_user_pml4`, `vmm_alloc_page`), `kernel/elf.c` (`elf_load_file`), `kernel/syscall.c` (sys_exec/sys_exec-chain)

## Masalah

`vmm_alloc_page()` memilih target PML4 lewat global `vmm_user_pml4` yang di-set
sebelum `elf_load_file()` dan di-clear sesudahnya. Dua CPU yang exec bersamaan:

```text
CPU A: vmm_user_pml4 = A
CPU B: vmm_user_pml4 = B
CPU A: vmm_alloc_page() → bisa memetakan ke AS milik B
```

Window set/clear juga bisa terpotong timer interrupt di CPU yang sama (task lain
exec saat preemption). Hasil: user page nyasar ke address space yang salah —
korupsi lintas-proses yang sulit dilacak.

## Rencana fix

1. Tambah `vmm_alloc_page_into(uint64_t vaddr, uint64_t flags, phys_addr_t pml4)`
   (inti `vmm_map_page_into` sudah ada — tinggal wrapper alloc).
2. Ubah `elf_load_file(path, stack_out, pml4_target)` — semua mapping user page
   memakai parameter eksplisit, bukan global.
3. Hapus `vmm_user_pml4` sepenuhnya; sesuaikan semua call site di `syscall.c`.
4. Tidak ada lock global baru — per-CPU ELF load boleh tetap paralel.

## Verifikasi

- Dua app di-exec nyaris bersamaan di `-smp 4` (loop exec dari dua task).
- PTE walk sampling: user page hanya muncul di PML4 pemiliknya.
- `grep vmm_user_pml4` → 0 hasil.

## Implementasi (selesai)

- API baru: `vmm_alloc_page_into(vaddr, flags, pml4)` +
  `paging_is_mapped_into(vaddr, pml4)`; `PHYS_NULL` → kernel PML4.
- `vmm_alloc_page` / `paging_is_mapped` jadi wrapper tipis (kernel PML4).
- `elf_load_file(..., target_pml4)` — semua mapping via parameter eksplisit
  (ELF64 & ELF32 legacy); caller `kernel_userlib.c` kirim `PHYS_NULL`.
- `syscall.c`: kedua jalur exec (25/33) meneruskan `new_pml4`; global dihapus.
- `sys_is_mapped` (44) kini memeriksa AS **task pemanggil**, bukan global.
- Runtime (`HEAP_WATCH_DEBUG`, `-smp 4`): 0 panic, 0 ELF error, PNG sukses.
- `grep vmm_user_pml4` → hanya 1 komentar historis. Stress paralel-exec
  dua CPU: eliminasi by construction (tidak ada shared mutable state).
