# Desain: Modul Scheduler `kernel/sched/`

> **Status**: SELESAI (2026-07-26) — refactor struktural murni, **tanpa
> perubahan perilaku**. Build bersih, boot `-smp 4` OK.
> **Asal**: pecahan dari `kernel/task.c` (907 baris, dihapus). Bagian dari
> seri de-fat-file yang sama dengan `kernel/smp/` dan `kernel/gfx/`
> (pecahan `kernel.c`).

## Tujuan

`kernel/task.c` mencampur lima subsistem berbeda dalam satu file: run queue
per-CPU, scheduler core (context switch), lifecycle task, blocking API, dan
debug dump. Pemecahan per-subsistem membuat setiap file punya satu tanggung
jawab — tanpa mengubah satu pun simbol publik: **`include/task.h` tidak
berubah**, dan tidak ada consumer (syscall.c, wait.c, timer.c, lapic.c, asm)
yang diedit.

## Peta file

| File | Isi | Simbol publik |
|------|-----|---------------|
| `kernel/sched/core.c` | Context switch + CR3 switch, idle loop, state global (`tasks[]`, `task_count`, `scheduler_lock`, `cpu_current_task[]`) | `schedule_on_cpu`, `schedule`, `scheduler_idle_loop`, `smp_current_task_id`, `yield`, `task_state_name` |
| `kernel/sched/runqueue.c` | Run queue per-CPU (FIFO + priority & aging), placement, work-stealing. `cpu_runqueues[]` **privat** di balik accessor | `runq_push`, `runq_pop`, `pick_target_cpu`, `steal_task`, `runq_len`, `runq_init` (internal, via `sched_internal.h`) |
| `kernel/sched/lifecycle.c` | `tasking_init`, `create_task(_prio)` (fake ISR frame), `task_exit`, idle/syscall stack permanen per-CPU (FIX_001/FIX_005) | `tasking_init`, `create_task`, `create_task_prio`, `task_exit`, `task_idle_stack_top`, `task_syscall_stack_top`, `task_exit_finish_on_idle` |
| `kernel/sched/block.c` | Blocking dua fase + sleep queue | `block_prepare`, `block_park`, `block_current_task`, `unblock_task`, `task_sleep_ms`, `sleepq_check_wakeups` |
| `kernel/sched/debug.c` | Dump state task & per-CPU (perintah shell) | `scheduler_dump` |
| `kernel/sched/sched_internal.h` | Kontrak internal antar file sched — **jangan** di-include dari luar `kernel/sched/` | — |

API publik satu-satunya tetap `include/task.h`.

## State bersama internal (`sched_internal.h`)

Hanya **dua** variabel yang tadinya `static` di task.c dinaikkan jadi global
(dipakai lintas file sched):

- `scheduler_lock` — guard alokasi slot TCB, `task_count`, transisi state.
- `cpu_current_task[SMP_MAX_CPUS]` — task id yang berjalan per CPU (-1 = idle).
  Ditulis hanya oleh CPU pemiliknya; baca remote bersifat advisory.

Sisanya tetap privat per-file: `cpu_runqueues[]` + `run_queue_t` (runqueue.c),
`idle_stack_tops[]` + `syscall_stack_tops[]` (lifecycle.c).

## Invarian yang dipertahankan (dan di mana tinggalnya)

1. **Single-membership run queue** — task READY ada di TEPAT SATU queue;
   task RUNNING tidak di queue mana pun (dilacak `cpu_current_task`).
   Mencegah satu task jalan di dua CPU. Dokumen: header `runqueue.c`;
   urutan pop-dulu-baru-requeue: `schedule_on_cpu` di `core.c`.
2. **Lock order**: `wq->lock` → `scheduler_lock` → `rq->lock`, tidak pernah
   dibalik. Catatan di `sched_internal.h` dan header `runqueue.c`.
3. **FIX_001 (UAF stack task DEAD)**: `task_exit` memindahkan ownership stack
   ke register lokal + zero `stack_base`, lalu pindah ke idle stack permanen
   sebelum `kfree`. Slot-reaper di `create_task_prio` adalah sisi lain dari
   invarian ini — keduanya **sengaja satu file** (`lifecycle.c`).
4. **Blocking dua fase**: `block_prepare()` + `block_park()` di bawah lock
   objek menutup lost-wakeup window (dipakai `kernel/wait.c`). Seluruh state
   machine-nya utuh di `block.c`.

## Constraint ABI (yang membuat pemindahan aman)

- `arch/x86/idle_switch.asm` `jmp` ke `scheduler_idle_loop` dan
  `task_exit_finish_on_idle` — keduanya harus tetap **global**, noreturn,
  argumen di RDI. Terpenuhi: keduanya extern di `core.c`/`lifecycle.c`.
- Layout `registers_t` / frame PUSHA64 tinggal di `include/task.h` +
  `arch/x86/isr_macro.inc` — tidak tersentuh refactor. `create_task_prio`
  (pembangun fake ISR frame) pindah utuh ke `lifecycle.c`.
- `schedule_on_cpu` wajib mengembalikan `registers_t*` di RAX (stub timer/
  LAPIC melakukan `mov rsp, rax`) — signature tidak berubah.

## Build

`Makefile`: `SRC_DIRS` ditambah `kernel/sched` (glob per-folder non-rekursif).
Perhatian saat memindah file dari `kernel/`: **`make clean` DULU sebelum file
.c dihapus/dipindah** — object lama (mis. `kernel/task.o`) tidak lagi tercakup
`$(OBJS)` setelah sumbernya hilang, dan akan menyebabkan duplicate-symbol
saat link. Regen IntelliSense: `make compile_commands` (butuh Python dengan
module `compiledb`).

## Verifikasi

- `make clean && make` — tanpa undefined/duplicate symbol (termasuk simbol
  yang dirujuk asm: `scheduler_idle_loop`, `task_exit_finish_on_idle`,
  `schedule_on_cpu`).
- Boot QEMU `-smp 4`: `[smp] Online CPUs: 4/4`, shell hidup, preemption jalan.
- `make conc` — uji langsung API yang dipindah (sleep/mutex/semaphore/condvar
  lewat `block_prepare`/`block_park`/`unblock_task`/`task_sleep_ms`).

## Catatan dead code (dilaporkan, tidak dihapus)

- `current_task` (core.c) — nol pembaca di seluruh repo; hanya ditulis.
- `schedule()` (core.c) — nol pemanggil; semua jalur pakai `schedule_on_cpu`.
- `block_current_task` (block.c) — hanya dipakai internal `task_sleep_ms`,
  tapi masih dideklarasikan publik di `task.h`.

Kandidat pembersihan di commit terpisah.
