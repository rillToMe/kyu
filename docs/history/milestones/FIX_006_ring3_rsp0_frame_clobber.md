# FIX_006 — Frame preemption ring-3 tertimpa di per-CPU syscall stack (RSP0 global)

> **Status:** DONE (2026-07-27) — fix: RSP0 mengikuti task (per-task kernel stack).
> **Prioritas:** P0 (BOSD deterministik saat ≥2 task ring-3 berbagi satu CPU).

## Gejala

Setelah Phase 5A (spawn), membuka 3 app berjalan normal, tapi app ke-4 →
**BOSD Page Fault**. Dua wajah crash yang sama-sama teramati:

1. Kernel fault di `viewport_render` — `vp->source` = 0x3EE2A (= nilai
   `pmm_total_pages`!) dan field `vp` lain sampah → frame kernel terkorupsi.
2. User fault `CR2=0` di RIP `0x400140B` — app melompat ke **padding
   antar-fungsi** (`test %al,(%rax)` dengan RAX=0) → register task sampah
   saat resume.

## Root Cause

`TSS.RSP0` hanya di-set SEKALI saat boot (`sched_stacks_init`,
`smp_ap_main`) — menunjuk TOP per-CPU syscall stack yang dipakai BERSAMA
untuk semua transisi ring-3→ring-0 di CPU itu.

Konsekuensinya (`timer_isr.asm` / `lapic_timer_isr.asm`): frame preemption
task ring-3 SELALU ditulis di region `[TOP-176, TOP]` yang sama, siapa pun
task-nya. Saat dua task ring-3 berbagi satu CPU, preemption task kedua
**menimpa frame task pertama yang terdampar**. Resume memakai frame sampah
→ register korup → BOSD.

Latent sejak ring-3 lahir (FIX_005 Tahap 1): model exec-chain hanya punya
SATU app ring-3, jadi tidak pernah ada dua task ring-3 di CPU yang sama.
Terekspos pertama kali oleh Phase 5A — app ke-4 = pertama kalinya dua app
ring-3 berbagi CPU (4 CPU, kmain + 4 app).

## Fix

**RSP0 mengikuti task** (pola standar per-task kernel stack):

- `kernel/sched/core.c` (`schedule_on_cpu`): tiap context switch,
  `tss_set_rsp0(cpu, tasks[next].stack_base + TASK_STACK_SIZE)`; task tanpa
  stack sendiri (`stack_base==0`, mis. kmain) tetap memakai per-CPU syscall
  stack (`task_syscall_stack_top`).
- `include/task.h`: `TASK_STACK_SIZE` 8192 → **16384** — stack task kini
  juga menampung rantai syscall ring-3 yang dalam (exec → ELF → KFS → ATA),
  disamakan dengan `SYSCALL_STACK_SIZE` yang sebelumnya menampungnya.

Migrasi antar-CPU otomatis aman: frame hidup di stack milik task sendiri —
valid dari CPU mana pun.

## Verifikasi (QEMU `-smp 4`, 2026-07-27)

Skenario yang sebelumnya 100% BOSD: `start` 6 app berurutan
(clock, fileman, calc, notepad, taskmgr, viewer) → semua hidup, window
render konkuren, shell responsif, stabil ≥16 detik soak. Sebelum fix:
BOSD deterministik di app ke-4.

## Catatan

- Batas konkurensi saat ini BUKAN bug ini: `MAX_TASKS = 8` (array TCB
  statis) → kmain + maks. 7 app spawned. Itu batas artifisial (desain
  "start simple"), bisa dinaikkan nanti; batas sebenarnya = RAM
  (~16KB kernel stack + AS + 256KB user stack + ELF + heap per task).
- `-MMD -MP` di Makefile (fix 5A) memastikan perubahan `TASK_STACK_SIZE`
  me-rebuild semua pemakainya.

## File yang berubah

| File | Isi |
|---|---|
| `kernel/sched/core.c` | RSP0 per-task di `schedule_on_cpu` |
| `include/task.h` | `TASK_STACK_SIZE` 16384 + komentar alasan |
