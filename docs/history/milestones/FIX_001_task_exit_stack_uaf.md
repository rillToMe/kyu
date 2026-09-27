# FIX 001 — `task_exit()` menjalankan idle loop di stack task DEAD

> **Prioritas**: P0
> **Status**: **DONE** (2026-07-26) — terverifikasi: `conc` 5/5 PASS di `-smp 4` + regresi jalur app bersih
> **Lokasi**: `kernel/task.c` — `task_exit()`, `scheduler_idle_loop()`, `create_task()` (slot reaper)

## Masalah

`task_exit()` menandai task `TASK_DEAD`, mengosongkan `cpu_current_task`, lalu
langsung `scheduler_idle_loop()` — **CPU tetap berjalan di `%rsp` stack task yang
sudah DEAD**. Di CPU lain, `create_task()` menemukan slot DEAD dan
`kfree(tasks[slot].stack_base)`. Blok stack itu masuk heap bebas, dapat dipakai
alokasi lain, sementara CPU pertama masih menaruh interrupt frame + locals di
sana setiap tick → UAF stack → korupsi heap/frame.

Ini race UAF stack task; penulisnya menulis lewat stack pointer, bukan lewat
`kmalloc`, jadi `heap_lock` tidak melindungi apa pun.

## Rencana fix

1. **Per-CPU permanent idle stack** (disarankan):
   - Alokasikan satu stack kecil per CPU saat `smp_init`/`tasking_init` (mis. 8–16 KB), **tidak pernah di-free**.
   - `task_exit()` berpindah ke idle stack CPU ini **sebelum** masuk idle loop
     (perlu trampoline asm kecil / naked helper: `mov rsp, idle_stack_top` lalu
     `jmp scheduler_idle_loop` — tidak bisa dilakukan aman dari C murni).
   - Setelah itu stack task DEAD bebas di-`kfree` kapan pun.
2. Alternatif (lebih lemah): deferred reclamation — tunda `kfree(stack_base)`
   sampai ada bukti CPU sudah pindah stack. Lebih rapuh; hanya fallback.

## Catatan desain

- `scheduler_idle_loop()` juga dipakai AP sebelum task pertama — idle stack
  bisa sekalian dipakai sebagai stack awal AP.
- Interrupt frame timer LAPIC tetap mendarat di stack aktif → pastikan idle
  stack cukup dalam untuk handler penuh.

## Verifikasi

- Stress spawn/exit ratusan task di `-smp 4` (bisa pakai target `make conc`/`stress`).
- Build `HEAP_WATCH_DEBUG`: tidak ada `[W0]`/panic; `sched` stabil.
- `create_task` slot reuse: stack lama ter-free **setelah** CPU pemiliknya
  tercatat berada di idle stack.

## Implementasi (selesai)

- `arch/x86/idle_switch.asm`: `task_switch_to_idle_stack` (jalur AP) &
  `task_exit_via_idle` (jalur exit) — `mov rsp` lalu `jmp` (noreturn).
- `tasking_init` mengalokasikan 16 idle stack (`SMP_MAX_CPUS` × 8 KB),
  tidak pernah di-free.
- `task_exit()`: ownership stack dipindah ke register (`stack_base = 0` di
  bawah `scheduler_lock`) → reaper tak bisa menyentuhnya; `kfree` baru terjadi
  di `task_exit_finish_on_idle` **setelah** RSP pindah ke idle stack.
- `smp_ap_main` ikut pindah ke idle stack (meninggalkan stack Limine).
