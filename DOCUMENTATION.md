# Kyuzen OS - Task & Multitasking Documentation

> **Arsitektur**: Preemptive Multitasking via PIT IRQ0 (BSP) dan LAPIC timer (AP)
> **Header**: `include/task.h`  
> **Implementasi**: `kernel/task.c`, `drivers/timer.c`, `arch/x86/timer_isr.asm`, `arch/x86/lapic.c`

---

## Recent Updates: Network Ping & Refresh Rate

### e1000 / Ping Networking

Kyuzen OS sekarang punya jalur networking awal berbasis **Intel e1000 + lwIP**:

```
shell ping
    └─→ sys_ping / kernel_ping()
            └─→ lwIP raw ICMP
                    └─→ kyuzen_netif linkoutput
                            └─→ e1000_send()
```

Perbaikan penting di driver e1000:

- TX/RX descriptor ring dan packet buffer sekarang dialokasikan dari **PMM physical pages**, lalu diakses CPU lewat **HHDM** (`phys + hhdm_offset`).
- Driver tidak lagi memakai `kmalloc()` untuk buffer DMA e1000, karena heap kernel berada di virtual mapping VMM (`0xFFFF9000...`) dan tidak bisa dikonversi benar dengan `virt - hhdm_offset`.
- Bug sebelumnya: NIC menerima alamat DMA palsu, descriptor TX tidak pernah selesai (`DD` tidak balik), lalu log penuh dengan `[e1000] WARN: TX ring full`.
- Setelah fix, test pertama yang disarankan adalah `ping 10.0.2.2` di QEMU user networking. Jika gateway reply, TX/RX e1000 + ARP + ICMP dasar sudah bekerja.

File terkait:

| File | Fungsi |
|------|--------|
| [`drivers/net/e1000/e1000.c`](drivers/net/e1000/e1000.c) | Driver Intel e1000, DMA descriptor ring, TX/RX poll |
| [`drivers/net/port/kyuzen_netif.c`](drivers/net/port/kyuzen_netif.c) | Glue layer e1000 ↔ lwIP |
| [`kernel/net_init.c`](kernel/net_init.c) | Init lwIP, netif, DHCP/static fallback, DNS |
| [`kernel/net_ping.c`](kernel/net_ping.c) | ICMP Echo Request/Reply implementation |
| [`apps/shell.c`](apps/shell.c) | Command shell `ping [host]` |

### Runtime Refresh Rate

Default timer/PIT refresh rate diubah dari **50Hz** menjadi **60Hz**. Refresh rate juga bisa diganti dari shell dengan preset awal:

```text
refresh
refresh 60
refresh 100
refresh 144
```

Behavior:

- `refresh` tanpa argumen menampilkan refresh rate aktif.
- `refresh 60`, `refresh 100`, dan `refresh 144` memprogram ulang PIT runtime.
- Nilai lain ditolak agar path awal tetap stabil.
- `timer_get_ms()` sekarang berbasis accumulator runtime, bukan konstanta compile-time, jadi uptime, sleep, lwIP timeout, dan scheduler tetap konsisten saat refresh rate diganti.
- Scheduler quantum tetap berbasis waktu **20ms**, sementara IRQ timer berjalan sesuai refresh rate aktif.

File terkait:

| File | Fungsi |
|------|--------|
| [`include/timer.h`](include/timer.h) | Default 60Hz, API `timer_set_refresh_rate()` dan `timer_get_refresh_rate()` |
| [`drivers/timer.c`](drivers/timer.c) | Program PIT runtime, accumulator waktu ms, CPU usage tracker |
| [`kernel/timer_callbacks.c`](kernel/timer_callbacks.c) | Callback visual flush, cursor, network poll |
| [`apps/shell.c`](apps/shell.c) | Command shell `refresh [60|100|144]` |

---

## Cara Kerja Singkat

Kyuzen OS menggunakan **Preemptive Multitasking**. BSP masih memakai timer PIT (IRQ0), sementara AP memakai LAPIC timer vector `0xF0` untuk tick scheduler awal. Timer menyimpan seluruh register CPU dari task yang sedang berjalan, lalu memberi scheduler kesempatan untuk melanjutkan task berikutnya. Tidak ada `switch_task()` manual — semua context switch terjadi otomatis via interrupt.

```
Timer IRQ0 (default 60Hz, bisa 60/100/144)
    └─→ timer_isr_stub (ASM)
            ├─ PUSHA64 (simpan semua register ke stack)
            ├─→ timer_handler(rsp)           ← C handler
            │       └─→ schedule(r)           ← pilih task berikutnya
            │               └── return RSP_baru
            ├─ mov rsp, rax                   ← GANTI STACK ke task baru
            ├─ POPA64                          ← restore register task baru
            └─ IRETQ                           ← lompat ke RIP task baru
```

Untuk AP/multicore awal:

```text
LAPIC timer vector 0xF0
    └─→ lapic_timer_isr_stub
            ├─ PUSHA64
            ├─→ lapic_timer_handler(rsp)
            │       └─→ schedule_on_cpu(cpu_id, r)
            ├─ mov rsp, rax
            └─ IRETQ
```

---

## SMP Load Balancing (Per-CPU Run Queue + Work-Stealing)

Scheduler memakai **satu run queue per CPU**, bukan satu antrian global. Setiap
run queue adalah FIFO berisi id task `TASK_READY` dan punya lock sendiri, jadi
CPU yang berbeda tidak saling menunggu di satu lock global pada jalur cepat.

Invarian inti: **sebuah task `TASK_READY` hanya ada di TEPAT satu run queue**, dan
task `TASK_RUNNING` tidak ada di queue mana pun (dilacak oleh `cpu_current_task`).
Inilah yang mencegah satu task berjalan di dua core sekaligus — untuk menjalankan
task, CPU harus men-`pop` task dari queue lebih dulu (menghapusnya dari semua
queue secara atomik di bawah lock queue).

Penyeimbangan beban punya dua sisi:

- **Placement** — `create_task` menaruh task baru ke CPU paling ringan
  (`pick_target_cpu`: core idle menang langsung, selain itu pilih beban terkecil),
  lalu hanya membangunkan CPU itu saja dengan satu IPI reschedule. Ini
  menggantikan pola lama yang membangunkan **semua** core idle untuk satu task
  (thundering herd) sehingga semua berebut lock hanya untuk satu yang menang.
- **Work-stealing** — saat run queue lokal kosong, `schedule_on_cpu` mencuri satu
  task dari run queue remote yang paling sibuk (`steal_task`). Task yang sedang
  `TASK_RUNNING` di core lain tidak pernah disentuh — hanya task yang menunggu.

Urutan operasi di `schedule_on_cpu` menjaga invarian di atas: `next` di-`pop`
lebih dulu (kepemilikan eksklusif diamankan), baru task keluar (`cur`) disimpan
konteksnya dan di-`push` kembali ke queue lokal agar core lain boleh mencurinya.

Aturan lock (bebas deadlock): `scheduler_lock` selalu lock terluar, lock per-queue
selalu di dalamnya, dan tidak pernah dua lock queue dipegang bersamaan (stealing
mem-`pop` korban lalu mem-`push` ke diri sendiri sebagai dua critical section
terpisah). Command shell `sched` (`scheduler_dump`) menampilkan panjang run queue
tiap CPU (`cpuN=...(q=M)`) untuk memantau keseimbangan beban.

---

## Quick Start — Membuat Task Baru

### 1. Tulis fungsi task kamu

```c
// Di file manapun (kernel/myfeature.c, dll)
#include "task.h"

void my_background_task(void) {
    while (1) {
        // Lakukan sesuatu...
        do_work();

        // Beri tahu timer bahwa kita sedang "idle" (untuk CPU usage tracker)
        // Timer akan preempt kita secara otomatis — yield() hanya opsional hint
        yield();
    }
    // Boleh return; trampoline scheduler akan memanggil task_exit().
}
```

### 2. Daftarkan task di `kernel/kernel.c` (atau setelah `tasking_init`)

```c
#include "task.h"

void kmain(void) {
    // ... inisialisasi hardware ...

    tasking_init();  // WAJIB dipanggil sebelum create_task

    // Buat task baru — langsung aktif!
    create_task(my_background_task, "background");
    create_task(another_task,        "another");

    // Lanjutkan kernel utama seperti biasa
    // Timer akan preempt dan switch otomatis
    sti();
    user_shell();  // Task 0 (kmain) terus berjalan
}
```

---

## API Reference

### `tasking_init(void)`
Inisialisasi subsistem tasking. Daftarkan kernel main (kmain) sebagai **Task 0**.  
**Harus dipanggil SEBELUM** `create_task()` dan **SETELAH** `init_timer()`.

```c
tasking_init();
```

---

### `create_task(func, name)`
Buat task baru dengan stack sendiri dan fake ISR frame.

| Parameter | Tipe | Keterangan |
|-----------|------|-----------|
| `func` | `void (*)(void)` | Fungsi entry point task |
| `name` | `const char*` | Nama task untuk debugging (max 15 karakter) |

```c
create_task(my_task_func, "my-task");
```

**Batas:** Maksimum `MAX_TASKS = 8` task secara bersamaan (termasuk kmain).  
**Stack size:** Tiap task mendapat `TASK_STACK_SIZE = 8192` bytes (8KB).

---

### `task_exit(void)`
Mengakhiri task saat ini dan menandainya sebagai `TASK_DEAD`. Task yang `return` dari entry point otomatis lewat trampoline dan masuk ke `task_exit()`.

```c
task_exit(); // tidak kembali
```

---

### `scheduler_dump(void)`
Debug helper untuk mencetak task aktif, CPU online, dan mapping CPU → task. Shell menyediakan command:

```text
sched
```

---

### `yield(void)`
Hint bahwa task sedang idle — CPU di-halt sampai interrupt berikutnya.

```c
yield();  // Hemat CPU, tunggu IRQ berikutnya (timer, keyboard, dll)
```

> **Catatan:** Dalam preemptive mode, `yield()` **tidak wajib** dipanggil.  
> Timer akan mengecek quantum scheduler berbasis waktu setiap tick.  
> Gunakan `yield()` di dalam loop menunggu untuk hemat daya CPU.

---

### `schedule(registers_t* current_regs)` / `schedule_on_cpu(cpu_id, regs)` ← Internal
Dipanggil secara otomatis oleh `timer_handler` (BSP) dan `lapic_timer_handler`/`lapic_reschedule_handler` (AP). **Jangan panggil langsung.** Memilih task berikutnya dari run queue lokal CPU, lalu work-stealing dari queue remote paling sibuk bila lokal kosong. Lihat [SMP Load Balancing](#smp-load-balancing-per-cpu-run-queue--work-stealing).

---

## Task States

```c
#define TASK_READY    0  // Siap dijadwalkan, menunggu giliran
#define TASK_RUNNING  1  // Sedang berjalan di CPU saat ini
#define TASK_SLEEPING 2  // Menunggu event (belum diimplementasi)
#define TASK_DEAD     3  // Selesai, slot bisa di-reuse
```

Untuk menghentikan task dari dalam task itu sendiri:

```c
void my_task(void) {
    do_work();
    task_exit();
}
```

---

## Memeriksa Status Task (Debug)

```c
#include "task.h"

scheduler_dump();
```

Atau dari shell:

```text
sched
```

---

## Contoh Lengkap: Background Counter Task

```c
// kernel/counter_task.c
#include "task.h"
#include "timer.h"

static uint64_t counter = 0;

void counter_task(void) {
    uint64_t last_print = 0;

    while (1) {
        counter++;

        // Print setiap 1 detik (tanpa busy-wait)
        uint64_t now = timer_get_ms();
        if (now - last_print >= 1000) {
            last_print = now;
            kprintf("[counter] %llu\n", counter);
        }

        yield();  // Beri giliran ke task lain, hemat CPU
    }
}
```

```c
// kernel/kernel.c
#include "task.h"

extern void counter_task(void);

void kmain(void) {
    // ... init hardware ...
    tasking_init();
    create_task(counter_task, "counter");
    // Selesai! counter_task berjalan paralel dengan kernel
}
```

---

## Arsitektur Teknis

### Stack Layout (Full ISR Frame)

Setiap task memiliki stack dengan "Fake ISR Frame" di bagian atasnya.  
Scheduler bekerja dengan cara **mengganti RSP** ke ISR frame milik task berikutnya.

```
Stack task (tumbuh ke bawah ↓)
┌─────────────────────────────┐ ← stack_top (alokasi kmalloc)
│  SS         = 0x10          │
│  RSP        = stack_top     │
│  RFLAGS     = 0x202 (IF=1)  │ ← CPU akan IRETQ dari sini
│  CS         = 0x08          │
│  RIP        = &trampoline   │ ← Wrapper aman untuk return/task_exit
│  error_code = 0             │
│  int_num    = 0             │
│  rdi        = &func         │ ← Argumen pertama trampoline
│  rax..r15   = 0/arg (regs) │
└─────────────────────────────┘ ← task.rsp (RSP yang disimpan di TCB)
```

### Quantum

Scheduler memakai quantum **20ms** (bisa diubah di `drivers/timer.c`). Timer IRQ sendiri berjalan sesuai refresh rate aktif (`60`, `100`, atau `144` Hz):

```c
// drivers/timer.c
#define SCHEDULER_QUANTUM_MS 20  // Ubah di sini untuk fine-tuning
```

### Batas & Keterbatasan Saat Ini

| Item | Nilai | Catatan |
|------|-------|---------|
| Max tasks | 16 | `MAX_TASKS` di `include/task.h` |
| Stack per task | 16 KB | `TASK_STACK_SIZE` di `include/task.h` (juga RSP0 task ring-3) |
| Timer refresh | 60 / 100 / 144 Hz | Default 60Hz, bisa diubah via shell `refresh` |
| Quantum | 20 ms | Scheduler quantum di `drivers/timer.c` |
| SMP scheduler | Per-CPU run queue | Load balancing: placement least-loaded + work-stealing (lihat "SMP Load Balancing") |
| Ring 3 (user space) | ⚠️ Belum | Semua task saat ini Ring 0 (kernel) |
| Task cleanup/join | ⚠️ Parsial | Task return masuk `task_exit`; stack lama di-reuse saat slot dipakai ulang |
| Stack overflow guard | ⚠️ Belum | Jangan allokasi array besar di task |

## Task Credentials (P0 Phase 1)

Identity lives in `task_t.cred` (`include/cred.h`: `uid` + `gid`), never in
a global. Rules:

- `tasking_init`: task0 = `(0, 0)` kernel/root.
- `create_task(_prio)`: inherit creator cred.
- `create_user_task` (spawn): inherit syscall caller cred; `-1` if no
  creator can be determined (never silent root).
- Accessors: `cred_current_uid/gid()`, `cred_current_is_root()`,
  `cred_task_uid/gid()` (`include/task.h`, defined in
  `kernel/sched/lifecycle.c`). Task-local reads, no locks.
- `sys_set_uid` (27): root-only transition of the caller's own cred
  (`uid`+`gid` move together). Non-root returns `-1`. Same policy in the
  Ring-0 shim (`apps/kernel_userlib.c`) — console shell cannot bypass it.
- Kernel-enforced root-only: `fs_format` (5), `shutdown` (38), `reboot`
  (39). Shell `sudo` flag stays as UX gate; the kernel check is the boundary.
- KWM ownership is task-id based (`owner_task == smp_current_task_id()`),
  unchanged — it never trusted UID.
- Shell `whoami`/`id`/prompt read `sys_get_uid`, now per-task; no changes
  needed beyond the new `sys_set_uid`/`fs_format` return codes.

NOT yet a POSIX model: no setuid bit, no sudoers (non-root `sudo`
elevation is denied by the kernel and reported), no password hashing,
no file ownership/modes, no `fork/argv/stdio/waitpid/kill`. Those are
later P0/P1 phases.

---

## Process Model (P0 Phase 2)

One authoritative model: `task_t` is the truth (`include/task.h`,
contracts in `include/proc.h`). No kernel-vs-userspace fake PID, no
shell-only argv, no separate stdio table.

### PID / parent

- PID = task slot id (`task_t.id`), stable while alive INCLUDING zombie.
  Reused only after the parent reaps (`ZOMBIE` → `DEAD`).
- `sys_get_pid` (45) stays a diagnostic per-AS cookie, NOT the PID.
  Use `sys_getpid` (70) / `sys_get_task_id` (43) for the PID.
- `parent_id` is set once at creation (`B.parent = A`), never inferred.
  Kernel tasks without a creator use `PROC_NO_PARENT (-1)`.
- Credentials (`uid/gid`, P0.1) and parenthood are separate: same UID
  never implies parenthood, and `waitpid` checks the parent link only.

### States

`READY / RUNNING / SLEEPING / BLOCKED / ZOMBIE / DEAD`. `ZOMBIE` keeps
pid + parent + exit status + name + cred until the parent reaps it.
The scheduler only picks `READY`, so zombies never run again.

### argv ABI

`main(int argc, char **argv)` via registers: `RDI = argc`, `RSI = argv`.
`argv[argc] == NULL`; `argv[0]` = app name. Old `void main()` apps ignore
`RDI/RSI` and keep working. Bounds: `argc` 1..16, each arg ≤64 bytes
INCLUDING NUL, total ≤512 bytes (`PROC_MAX_ARGC / PROC_MAX_ARG_LEN /
PROC_ARG_TOTAL_MAX`). Kernel validates everything; over-long input fails
the spawn (`-1`), never overflows. `argv` strings + pointer array live on
the child's own user stack (below `USER_STACK_TOP`, 16-byte aligned).

- `start app` → `argc=1`. `start app a1 a2` → `argc=3`.
- Old `sys_spawn(path)` → `argc=1, argv[0]=basename(path)`.
- New `sys_spawn_argv(path, argc, argv)` (68) for full argv.

No environment variables, no `PATH` lookup yet (`/apps/` prefix only).

### fd 0/1/2

Per-task, VFS-backed (`kernel/vfs_fd.c`, same table/locks/owner rules as
files — not a second subsystem). At creation each task gets
`0=stdin` (read-only), `1=stdout`, `2=stderr` (write-only), all routed to
the shared console TTY. `read(0)/write(1)/write(2)` work via syscalls
48/49. `close` on a stdio fd just releases that number (a later `open`
may reuse it — standard behavior future redirection relies on).
Each stdio fd has its OWN open description (stdin is `O_RDONLY` while
stdout/stderr are `O_WRONLY` — sharing one description would merge the
permission flags); all three route to the same console device.
`vfs_dup`/`vfs_dup2` work on every descriptor kind (see below).
GUI Terminal output stays callback-based (`TextEdit`); spawned-app
`stdout/stderr` goes to the shared console, NOT back into the spawning
terminal instance (documented boundary, per-terminal routing is future).

### dup / dup2 (P0 Phase 4)

FD entries are task-local; open descriptions are shared, reference
counted heap objects owning buffer + offset + flags (`kernel/vfs_fd.c`).

- `sys_dup` (74): `dup(oldfd)` → lowest free fd on the SAME open
  description (one offset, one buffer). All kinds duplicable.
- `sys_dup2` (75): `dup2(oldfd, newfd)` → `newfd` on the same
  description; `oldfd == newfd` is a validated no-op; an open `newfd`
  is closed first (new reference acquired before the target is
  released, so self-sharing ends can't destroy the description).
- `read(fd3)` then `read(fd4)` on duplicates advances ONE shared
  offset (verified by `fd_test`: AB via fd3, CD via fd4, E after
  closing fd3).
- `close` drops one reference; only the last close flushes (dirty
  files) + frees. `proc_exit` releases each fd the same way, so one
  task's exit never invalidates another task's references.
- Lifetime boundary is the existing `vfs_lock` (table + refcount +
  file data path); TTY device I/O holds a temp reference across its
  lock drop so concurrent close can't free mid-I/O. No new lock.
- Spawn inherits nothing by default (fresh stdio, file fds stay with the
  opener); explicit per-fd stdio inheritance via `sys_spawn_redir` (77)
  for shell redirection/pipelines — whole-table sharing is fork's job
  (see 6B section).

### Pipes (P0 Phase 5)

One pipe = one `vfs_pipe_t` (bounded circular buffer, `VFS_PIPE_CAP` =
4096 bytes) shared by exactly one `PIPE_READ` and one `PIPE_WRITE` open
description (`kernel/vfs_fd.c`). `dup()` aliases the same endpoint
description (side-alive flags, not fd counts: two read dups are still
one reader). Direction enforced by description flags + kind.

- `sys_pipe` (76): `pipe(fds)` → `fds[0]=read, fds[1]=write`, both in the
  caller table. Both-or-neither: table full → `-1`, nothing allocated.
- Blocking (no busy-wait) on the pipe's own wait queue (its lock guards
  all pipe state; order `vfs_lock` → pipe lock, never reversed):
  empty + writers alive → reader blocks; full + readers alive → writer
  blocks; wake-all on every state change, condition re-checked in a loop
  (two-phase block via `wait_block_killable`, same primitive as
  waitpid/mutex — no second framework, no lost wakeups).
- EOF: empty + no writers → `read` returns 0 (sticky). Kill of a blocked
  task reports through `wait_block_killable` so its temp description
  reference is released before `proc_exit_kill` (no leak, no UAF).
- Broken reader: no readers → `write` returns `-1`. No `SIGPIPE` by
  design — the writer fails, it is never killed.
- Partial writes allowed (`min(count, free)`); readers get up to `count`.
  `lseek` on pipes fails. `close`/`proc_exit` drop one side reference;
  last side out wakes the other side and frees the pipe.
- `sys_spawn_redir` (77): like `sys_spawn_argv` plus `spec*`
  (`spawn_stdio_t`: caller fds for the child's 0/1/2, `-1` = fresh TTY).
  Installed after slot assignment, before the child is schedulable.
  Retained: `start`, console-shell pipelines (no user AS to fork), and
  the single-image `exec_load_image` loader is shared with spawn.
- `sys_execve` (79, P0 Phase 6C): atomic in-place image replace —
  build-new-AS-first, then swap AS/heap/argv/trap-frame and destroy old.
  Same pid/ppid/creds/fds/windows; fresh stack/entry/argv; GP regs
  zeroed. Failure returns `-1` with the old image untouched. Shell
  pipelines use fork → dup2 → execve (spawn_redir fallback where fork
  is unavailable).

### Shell redirection + pipelines (P0 Phase 5)

In `apps/shell_core.c` (shared by console + GUI terminal), no new parser:
operators must be separate tokens (`a | b`, `cmd > f`; `a>b` stays one
word). External apps only — stage names resolve to `/apps/*.elf`
(external wins over same-name builtins, e.g. `echo` → `echo.elf`);
builtin-only names and `start` in stages are rejected with an error.

- `cmd > f` (`O_WRONLY|O_CREAT|O_TRUNC`), `cmd >> f` (`O_APPEND`),
  `cmd < f` (`O_RDONLY`): parent opens the file, spawns the child with
  `sys_spawn_redir`, closes its copy, then joins (foreground). The
  shell's own stdio is never touched. `<` only in the first stage,
  `>`/`>>` only in the last; stderr always stays on console.
- `a | b [| c [| d]]` (max 4 stages): per stage the shell `fork()`s;
  the child wires stdin/stdout with `dup2`, closes every pipe end and
  redir file, then `execve`s the stage image (exit 127 on setup/exec
  failure — never returns to the shell loop). Kernel-context shells
  (console task 0: no user AS to clone) get fork `-1` and fall back to
  the legacy `spawn_redir` path with identical wiring. The parent closes
  ALL its copies immediately (this close discipline is what delivers
  EOF), then joins every child. Each child only ever holds the ends it
  needs, so EOF propagates when the writer exits.
- Join is uniform for both shells: poll `sys_proc_list` (presence +
  ppid==self pins our children against PID reuse; zombie snapshots give
  exit codes), reap via `waitpid` for authoritative statuses (no-op `-1`
  for auto-reaped task-0 children), 20 ms slices, ~60 s cap. From the
  console (task 0) absence alone means done, statuses unreported; from
  the GUI terminal (spawned task, real parent) children zombie normally
  and the shell reports nonzero exits.
- New CLI utils (hidden from desktop): `echo.elf` (argv → stdout),
  `cat.elf` (files or stdin → stdout). Examples: `echo hello | cat`,
  `echo hello > f1.txt`, `cat < f1.txt`, `cat f1.txt | cat`.

### GUI terminal E2E (P0-FINAL)

`user_apps/terminal.c` is a frontend only: it owns a KWM window +
`TextEdit` transcript and delegates every command to the one shared
engine (`apps/shell_core.c`) — no second shell implementation. Input
path: PS/2 → KWM focus (`focused_win_id`, set on window create and
click-to-focus) → `kwm_route_keyboard` → the terminal task's event
queue → `sys_get_event` → widget. Ctrl+C is intercepted as a window
shortcut (idle) or via the join `poll_input` (busy), so it never lands
as a literal `c`. Because the focused window owns the keyboard, typed
input correctly goes to the terminal and NOT to the console shell.

Serial observability for tests only: built with `-DTERM_SERIAL_MIRROR`,
the terminal echoes its transcript through `sys_write_fd(1, …)` (the
implicit-stdio TTY path, which already mirrors to COM1). The macro is
off in production builds — a no-op. No kernel change and no second
shell.

`qemu-gui-test.ps1` (4 CPUs, headless, QMP key injection, login →
`start terminal`) drives the real GUI terminal and verifies, all on the
mirrored transcript: external fork+execve (`echo alpha beta`), argv,
stdout, pipeline, 3-stage pipeline, `>`/`>>`/`<`, child failure
(`pipe: exit tidak nol`), Ctrl-C with no fg, Ctrl-C on a sleep-blocked
writer + pipe-blocked reader, Ctrl-C on a 3-stage pipeline, shell
survives + prompt returns after each, `jobs` → `(tidak ada child)` (no
zombie, no stale fg pid), and a 12× pipeline sweep (no pipe/FD leak —
the shell's 16-entry fd table would exhaust on any leak).

### Legacy / canonical path audit (P0-FINAL)

| Path | Callers | Status |
|---|---|---|
| `exec_load_image` | `spawn_common`, syscall 79 | **canonical** — one ELF path, shared by spawn + execve |
| `spawn_common` (77/68/57) | `sys_spawn_*` | **canonical** — `start`, redir, console fallback |
| `create_user_task` | `spawn_common` | **canonical** — only Ring-3 task factory |
| `task_fork` | syscall 78 | **canonical** |
| `proc_exit` / `proc_exit_kill` | syscall 34, faults, kill path | **canonical** — one `proc_do_exit` body |
| `vfs_fork_inherit` | `task_fork` | **canonical** — fork FD clone |
| `vfs_dup` / `vfs_dup2` | syscalls 74/75 | **canonical** — shell pipeline wiring |
| syscall 33 `sys_exec` | `apps/shell.c`, `fileman.c` launcher | **legacy, retained** — self-replacement that intentionally destroys the caller's windows (launcher → app). Non-atomic (destroys AS before loading the new one); touches only `self`, no shared/other-task state, so it cannot corrupt the P0 lifecycle. New code uses 78+79. |

Nothing legacy was deleted: 33 and `spawn_redir` remain for the
launcher and the kernel-context (task-0) fallback, both with their
original semantics.

### Foreground Ctrl-C (P0 Phase 6A)

This is NOT POSIX SIGINT: no signals, handlers, masks, groups, or PTY —
one input event (`^C` + prompt) mapped onto the existing
`sys_kill` → `proc_kill` → `proc_exit_kill` chain. No new termination
primitive, no second architecture.

- Detection: PS/2 driver cooks Ctrl+C (either side) into one ETX byte
  (`0x03`) for the TTY buffer only (`drivers/keyboard.c`); all other
  Ctrl/Alt combos stay excluded as before. GUI events already carry
  modifiers, so the terminal needs no driver change.
- Console: `0x03` at an empty prompt prints `^C` + fresh prompt (shell
  survives, nothing killed). During a pipeline the line loop is parked
  inside the join, so the join's `poll_input` (non-blocking TTY drain)
  consumes `0x03` as the interrupt; other type-ahead is discarded
  (documented: no type-ahead while a pipeline runs).
- GUI terminal: a `Ctrl+C` window shortcut (both cases) intercepts the
  keypress pre-widget when idle — no stray `c` in the input line; it
  clears the current line, prints `^C`, reprompts. During a pipeline the
  event loop is parked, so the join's `poll_input` drains the task's
  event queue for the combo (other queued events are dropped —
  re-click after the pipeline if a window event was swallowed).
- Tracking: fg pids live in `run_stages` locals (per shell instance by
  construction — console and every terminal have their own `shell_s`;
  no global PID, no cross-terminal effect). Cleared by scope exit.
- Interrupt: one `sys_kill` per pid, once (kill-requested flags; P0.3
  idempotence covers exit-raced/duplicate presses). Kill of one side
  also unblocks the other via existing pipe close/wake (producer death
  → consumer EOF; consumer death → producer `-1`), but the shell still
  kills the whole list rather than relying on pipe errors.
- Limits (carried, not introduced): a foreground app reading TTY stdin
  races the join poll for the `0x03` byte — retry; `reap`-blocked or
  password-prompt Ctrl-C is unhandled (byte buffered / auth fails, shell
  survives). Pure-compute loops with no syscalls **are now** observed —
  at the timer preemption (`proc_observe_kill_sched`, P0-FINAL), so the
  former "no boundary yet" debt is closed.

### fork() (P0 Phase 6B)

`sys_fork` (78): duplicate the caller. Parent gets the child pid, the
child resumes after the fork trap with `0`, `-1` on failure. No `exec`
follows in this phase (use `start fork_test`; the shell is NOT
converted to fork/exec).

- Strategy: FULL PHYSICAL COPY (not COW). COW was rejected on audit:
  no page refcounts exist, `#PF` always panics (no write-fault hook),
  and TLB shootdown is local-only — remote COW invalidation is unsolved
  debt. `vmm_clone_user_as` (`kernel/paging.c`) clones PML4[0..255]
  page by page (fresh frame + 4KB HHDM copy + identical flag bits);
  kernel halves [256..511] are shared by value, never deep-copied;
  huge/empty entries are skipped exactly like
  `vmm_destroy_address_space`. Parent tables are only read (no parent
  TLB work); child tables were never loaded (no invalidation needed).
  All current user mappings are plain 4KB `flags=7` (no W^X policy to
  preserve beyond verbatim copying); no framebuffer/device/shared
  mappings exist in the user half (verified: only ELF/stack/heap/argv
  mappers call into user-range mapping).
- Context: the child's kernel stack carries a verbatim copy of the
  parent's syscall trap frame (`registers_t`, 1:1 with `isr_macro.inc`)
  with `rax` forced to 0 — it returns through the normal POPA64+iretq
  path. Kernel stack never shared. No FPU/SSE context exists anywhere
  (toolchain-wide `-mno-sse`), so none is cloned. TSS syscall stacks
  are per-CPU, unaffected.
- Task: dedicated `task_fork` (`kernel/sched/lifecycle.c`) — slot via
  the existing DEAD-scan, creds via `cred_inherit`, parent = caller,
  fresh AS cookie, argv pointer valid via the cloned stack, name copied
  from parent. Published to the runqueue only when fully built (same
  pid-unknown-until-return argument as spawn). Rollback on every step:
  failed clone/stack/slot/uheap/fd/runq destroys the partial child
  (AS destroyer frees frames+tables) and returns `-1`.
- FDs: `vfs_fork_inherit` copies the whole table at the same numbers
  onto the SAME open descriptions (`open_get` each; pipe side flags
  untouched — dup-aliasing already proved this accounting). Child
  table must be empty or fork fails without clobbering. stdio shared
  (unlike spawn's fresh TTY). Close/exit/kill paths need no changes —
  they already drop exactly one reference.
- Heap: `uheap_clone` duplicates the kernel-side region list + `brk`;
  pages ride along at the same vaddrs inside the AS clone. Parent and
  child allocate/free independently afterwards.
- KWM: child starts with zero windows (ownership is per-task; the
  parent keeps its own; exit paths tear down per owner as before).
- Kernel contexts (no user AS, incl. Ring-0 console) are rejected
  (`-1`); Ring-3 is required (forged CS/SS must be user segments).
- `start fork_test`: return values, PID/PPID, creds, data-segment
  independence (`x==10` vs `20`), fd sharing via shared file offset
  (`PARENT`+`CHILD`), pipe inheritance with EOF, two bounded forks,
  kill of a sleep-blocked child (status 125).

### exit / wait

- `sys_exit` (34) now takes `RBX = code` (`void sys_exit()` = exit 0,
  `sys_exit_code(n)` explicit). Syscall `-1` ≠ exit `-1`: only this path
  records `exit_code`.
- Spawned apps terminate in `proc_exit` (`kernel/proc.c`): close FDs,
  flush event queue, destroy own KWM windows, free AS/stack/uheap, then
  `ZOMBIE` (live non-reaper parent) or `DEAD` (orphan / no parent).
  Termination and reclamation are separate: the slot is reusable only
  after reap.
- `sys_waitpid` (69): `waitpid(pid, &status, 0)`. Parent-only (non-child
  → `-1`). Blocks with the existing wait-queue primitives (no polling);
  child exit on any CPU wakes a parent on any CPU (`proc_wq` lock →
  `scheduler_lock` order, two-phase block, wake-all + re-check).
  `options` must be 0 (no `WNOHANG` yet). `waitpid(-1,...)` reaps any child.
- Orphans: children reparented to task 0 (kernel reaper placeholder) at
  parent exit; task 0 never waits so its adoptees auto-reap (`DEAD`,
  no zombie accumulation). No init/PID1 yet.
- `sys_proc_list` (72) returns a read-only `proc_info_t` snapshot
  (pid/ppid/state/uid/gid/exit_code/name) for Task Manager. Userspace
  cannot mutate task state through it.

### Syscalls added/changed

| # | Name | Args | Notes |
|---|------|------|-------|
| 34 | `sys_exit` extended | `RBX=code` | old `void` wrapper now passes 0 |
| 57 | `sys_spawn` kept | `RBX=path` | `argc=1`, basename `argv[0]` |
| 68 | `sys_spawn_argv` NEW | `RBX,RCX=argc,RDX=argv` | bounded, `-1` on bad input |
| 69 | `sys_waitpid` NEW | `RBX=pid,RCX=status*,RDX=0` | parent-only, blocking |
| 70 | `sys_getpid` NEW | — | task id |
| 71 | `sys_getppid` NEW | — | parent or `PROC_NO_PARENT` |
| 72 | `sys_proc_list` NEW | `RBX=buf,RCX=max` | count or `-1` |
| 74 | `sys_dup` NEW | `RBX=oldfd` | newfd on same open description / `-1` |
| 75 | `sys_dup2` NEW | `RBX=oldfd,RCX=newfd` | newfd (no-op if equal) / `-1` |
| 76 | `sys_pipe` NEW | `RBX=fds*` | 0, `fds[0]=read,fds[1]=write` / `-1` |
| 77 | `sys_spawn_redir` NEW | `RBX,RCX=argc,RDX=argv,RSI=spec*` | child pid / `-1` |
| 78 | `sys_fork` NEW | — | child pid (parent) / 0 (child) / `-1` |

### Test apps

- `start procinfo hello world` → prints PID/PPID/UID/GID/`argc=3` and
  each `argv[i]` (plus one line via fd 1).
- `start exit_test 42` + `waitpid` → parent receives 42 (real kernel
  lifecycle, not faked in userspace).
- `start fd_test` → open/dup/shared-offset/dup-survives-close/dup2/
  invalid-fd checks against the real kernel fd layer, exit 0 on PASS.
- `start pipe_test` → pipe create/direction/write-read/dup-share/EOF/
  broken-reader/invalid plus a 32KB parent↔child SMP roundtrip with byte
  verification, exit 0 on PASS.
- `start fork_test` → fork returns, PID/PPID, creds, memory independence,
  fd sharing, pipe inheritance + EOF, two bounded forks, blocked-child
  kill (125), exit 0 on PASS.
- Shell: `echo hello | cat`, `echo hello > f1.txt`, `cat < f1.txt`
  (external `echo.elf`/`cat.elf`; builtins can't do fd I/O).

### NOT implemented (explicit)

`exec`/`execve` (only spawn; shell NOT converted to fork/exec),
signals (incl.
`SIGPIPE`: broken-pipe write fails `-1` instead), process
groups/sessions, PTY/TTY per-terminal routing, job control (no `&`),
`2>` stderr redirection, env vars,
full init/PID1 (only the task-0 reaper placeholder), whole-table fd
inheritance on spawn (only explicit 0/1/2 via `sys_spawn_redir`),
COW/shared memory/`mmap`/ASLR (fork is full-copy), `vfork`.

---

## Process Termination & Kill (P0 Phase 3)

One authoritative termination path, unchanged: every death converges on
`proc_transition_locked()` in `kernel/proc.c` (reparent → ZOMBIE/DEAD +
detach stack/AS/heap), reached via `proc_exit()` (normal) or
`proc_exit_kill()` (kill convention). There is no second cleanup
implementation. `proc_exit()` is guarded: a racing transition (already
`ZOMBIE`/`DEAD`) performs no second cleanup.

### Termination audit (what can stop a task)

- `proc_exit(code)` — self exit, authoritative (spawned tasks via
  `sys_exit` 34 / `task_exit`).
- `sys_exit` (34) kernel exec-chain path — legacy longjmp to `user_shell`
  (task reused, no zombie; spawned path exits via `proc_exit` instead).
- `sys_kill` (73) — the only remote termination (see below).
- Scheduler removal — tasks leave the CPU via `proc_exit`'s
  `task_exit_via_idle`; `DEAD` slots are reused by `create_task(_prio)` /
  `create_user_task` (stale stack freed, heap metadata + event queue
  reset, `kill_pending` cleared).
- Cleanup fans out from the single path: `vfs_close_all`,
  `flush_event_queue`, `kwm_destroy_windows_of`, AS destroy, stack free.
  All three id-cleanup helpers are remote-safe (task-id param + own
  lock), which is what makes synchronous READY-kill possible.

### Kill model (NOT POSIX signals)

`sys_kill(pid)` (73, `RBX=pid`) → `0` ok / `-1` denied. No signal
numbers, handlers, groups, or sessions. Killed children report the
documented KyuzenOS convention (NOT 128+signo, no `W*` macros):

- `exit_code == PROC_KILL_EXIT_CODE (125)`, `exit_reason == PROC_EXIT_KILLED`.
- Normal exits keep their plain code with `PROC_EXIT_NORMAL`.
- `waitpid` callers compare status with `==`; `proc_list` exposes the
  authoritative `exit_reason` per zombie.

### Authorization (`proc_can_kill`, `include/proc.h`)

Kernel-enforced, userspace UID never trusted (`cred_current_is_root()`):

- Normal user: own child only (`target.parent_id == caller`).
- Root: any `TASK_KIND_SPAWNED` task.
- Suicide (`pid == self`): allowed for spawned tasks, exits directly.
- Always denied: PID 0, any `TASK_KIND_KERNEL` task (even for root),
  out-of-range PIDs, `DEAD`/`ZOMBIE` slots (second kill fails, never
  double-cleans). Double kill while pending returns `0` (idempotent).

### Sync vs async termination (`proc_kill`, `kernel/proc.c`)

- `READY` target — synchronous: purged from every run queue
  (`scheduler_remove_task`, holds `scheduler_lock`; lock order
  `scheduler_lock` → rq lock, same as `schedule_on_cpu`), then the
  shared transition + `vfs/event/kwm` cleanup + AS destroy + `kfree`
  run in the killer's context. Safe: `READY` = in exactly one queue,
  on no CPU, AS loaded nowhere. The task never runs again. Parent is
  woken via `proc_wq` like a normal exit.
- `BLOCKED`/`SLEEPING` target — async: `kill_pending = 1` set under
  `scheduler_lock`, then `unblock_task()` wakes it. It self-removes
  from its wait queue (lock held, no stale entry) and calls
  `proc_exit_kill()`. Covered queues: mutex/semaphore/condvar/`waitpid`
  (all block via `wait_block_locked`, which checks pre-park AND
  post-wake), `task_sleep_ms` (checks on return).
- `RUNNING` target (incl. another CPU) — async: flag + reschedule IPI
  to the running CPU (`smp_mark_reschedule` + `lapic_send_reschedule`;
  hint only, correctness never depends on it). No remote stack/AS
  memory is ever freed. The target observes at its next safe boundary:
  syscall entry/exit checks in `syscall_handler`, any block point, or
  the next preemption (see below), then `proc_exit_kill()`.
- Pure-CPU-bound target — observed at the next timer preemption
  (`proc_observe_kill_sched`, P0-FINAL): see "Kill observation at
  preemption" below. No syscall is required.

### Kill observation at preemption (`proc_observe_kill_sched`, P0-FINAL)

The last functional gap was a userspace task that loops forever with
**no syscalls**: it never reached a syscall/block boundary, so
`kill_pending` was never seen. Closed by reusing the scheduler's
existing preemptive ISR-frame path instead of adding any new probe:

- Hook point: the very top of `schedule_on_cpu(cpu_id, current_regs)`
  (`kernel/sched/core.c`), which is entered from the PIT tick
  (`timer_handler`), the LAPIC tick (`lapic_timer_handler`, APs), and
  the reschedule IPI (`lapic_reschedule_handler`). It runs **before any
  scheduler lock** is taken, on the preempted task's own kernel stack,
  in interrupt context — so the full exit (which takes vfs/kfs/paging
  locks, all `irqsave` leaf-ordered exactly like the normal self-exit
  path) cannot self-deadlock.
- Gate (cheap no-op otherwise): `regs->cs & 3 == 3` (the victim is in
  Ring 3 → holds no kernel locks), task is this CPU's current
  `TASK_RUNNING` `TASK_KIND_SPAWNED` task, and `kill_pending` is set.
  Then `proc_exit_kill()` (noreturn). Kernel-mode victims keep flowing
  to the existing syscall-exit observation; `BLOCKED`/`SLEEPING`
  victims belong to the unblock machinery and are never diverted here.
- Safety: the victim's AS/stack are its own (identical to the normal
  `proc_exit` self path — never a remote free), and `proc_do_exit`
  revalidates + single-transitions, so a concurrent killer/exit can
  only win the race cleanly (the loser parks forever). Stale reads are
  harmless both ways: a missed flag retries on the next tick (20 ms
  quantum); a just-set flag is owed death anyway.
- `proc_exit_kill()` still converges on the one authoritative
  `proc_do_exit` body; `waitpid` returns `PROC_KILL_EXIT_CODE` (125)
  and the slot is reaped exactly once. SMP-safe: observation happens on
  the target CPU, in the target's context, with no cross-CPU frees.

### Runqueue / scheduler notes

- `runq_remove(cpu, task)` (`kernel/sched/runqueue.c`) removes every
  occurrence of an id from one queue; `scheduler_remove_task(task)`
  (`kernel/sched/core.c`) purges all CPUs. READY-only by construction:
  `RUNNING` tasks live in no queue (`cpu_current_task` is owned by its
  CPU and never touched remotely); `BLOCKED`/`SLEEPING` tasks are in
  no queue until `unblock_task` requeues them — at which point the
  kill observation fires before any user code runs.
- The scheduler's existing stale-entry defense (`state != READY` →
  drop) is unchanged and covers any purge-vs-steal race.

### waitpid / reap interaction

`kill(child)` → child terminates → `ZOMBIE` → `waitpid(child)` returns
child pid + `PROC_KILL_EXIT_CODE` → slot reaped exactly once (second
`waitpid` → `-1`). A parent blocked in `waitpid` wakes via the normal
`proc_wq` wake-all (no polling, no missed wakeup: the kill path reuses
`unblock_task` + the wait re-check loop). Zombie slots stay occupied
until reap (`PID` reuse only after reap; stale-PID wait is `-1`).
Orphans still reparent to task 0 (killed parents included); task-0
children auto-reap (`DEAD`, e.g. console-shell spawns from task 0 never
zombify). No `WNOHANG` (deferred: needs no architectural change, just
not needed yet — shell does explicit `jobs`/`reap` instead, no polling).

### Shell (`jobs` / `reap` / `kill`, `apps/shell_core.c`)

`start` stays asynchronous (no behavior change, no background reaper,
no polling). Explicit opt-in builtins:

- `jobs` — read-only list of own children (`proc_list` filtered by
  `ppid`, state + name).
- `reap <pid>` — blocking `waitpid` on one child, prints status
  (`(killed)` for `PROC_KILL_EXIT_CODE`). Explicit waits only; never
  blocks the interactive shell unless the user asks.
- `kill <pid>` — `sys_kill` wrapper (kernel authorizes).

### Task Manager (`user_apps/taskmgr.c`)

Process table (`PID/Nama/UID/State`, `ZOMBIE(125)`-style reason suffix)
via read-only `sys_proc_list`, manual `Refresh` button (no selection
churn from auto-refresh; meters still tick at 2 Hz), `Kill` button on
the selected row via `sys_kill` only (never mutates task state
directly). Status line reports success / denied-already-exited /
no-selection, and refuses self-kill. Table refreshes after a
successful kill (zombies vanish once the parent reaps).

### Syscalls added

| # | Name | Args | Notes |
|---|------|------|-------|
| 73 | `sys_kill` NEW | `RBX=pid` | `0` / `-1`, parent-or-root policy |

`sys_kill` wrapper in `apps/userlib.c` + `apps/kernel_userlib.c`
(same return convention). No kernel pointers exposed.

### Tests

- Host `make test-kill` (`test/kill_test.c`, 18 groups): policy matrix
  (`proc_can_kill`), PID-0/kernel-task denial, self-kill, sync-READY
  purge, async blocked wake, kill status/reason, reap-once, double-kill
  single-transition, orphan reparenting, PID-reuse-after-reap, stale PID,
  UID≠parenthood. `test-cred` (7) + `test-proc` (17) still green.
- QEMU `kill_test.elf` (`start kill_test`, 4 CPUs): spawns a
  sleep-blocked child cross-CPU, kills, `waitpid` expects status 125,
  rejects `kill(0/99/-2)`, rejects double reap + stale wait; a
  window-holding child (`win=1`) killed to exercise KWM teardown; and
  (P0-FINAL) a **pure CPU-bound `spinchild`** (userspace loop, no
  syscalls) killed solely by the timer-preemption observation —
  single (`spinkill ok`) and in a mixed burst of two `spinchild` +
  one sleeping victim (`multikill ok`). Prints `kill_test: PASS`.
- Concurrency suite (`make conc`, 4 CPUs): sleep/semaphore/mutex-SMP/
  condvar/priority/vfs-fd all PASS — wait/block changes regression-free.
- Task Manager: launch → 15 s tick + `proc_list` refresh → ESC close →
  shell healthy, no panic (Kill-button mouse clicks remain manual).

### NOT implemented (explicit, carries over)

Signals/`SIGTERM` (incl. `SIGPIPE`), handlers, groups/sessions,
`WNOHANG` (tiny future extension, debatable need), PID1/init,
scheduler rewrite. The former hard-preempt ceiling (kill of a
no-syscall compute loop observable only at the next trap) is **closed**
by `proc_observe_kill_sched` at the schedule boundary — no IPI-redirect
design was needed.

---

## Files yang Relevan

| File | Fungsi |
|------|--------|
| [`include/task.h`](include/task.h) | Public API, `registers_t`, `task_t`, konstanta |
| [`include/cred.h`](include/cred.h) | `cred_t`, inherit + root-only transition policy |
| [`kernel/sched/lifecycle.c`](kernel/sched/lifecycle.c) | `create_task`, cred init/inherit/accessors |
| [`kernel/sched/core.c`](kernel/sched/core.c) | `schedule_on_cpu`, `smp_current_task_id` |
| [`include/timer.h`](include/timer.h) | Timer API, default refresh rate, preset runtime |
| [`kernel/task.c`](kernel/task.c) | Implementasi `create_task`, `schedule`, `yield` |
| [`drivers/timer.c`](drivers/timer.c) | `timer_handler`, PIT runtime refresh, scheduler quantum |
| [`kernel/timer_callbacks.c`](kernel/timer_callbacks.c) | Timer subscribers: visual, cursor, screen flush, network poll |
| [`arch/x86/timer_isr.asm`](arch/x86/timer_isr.asm) | ISR stub — inti dari context switch |
| [`arch/x86/isr_macro.inc`](arch/x86/isr_macro.inc) | `PUSHA64`/`POPA64` — layout stack frame |

---

## VS Code IntelliSense Setup (untuk Kontributor)

Build (`make`) sudah tahu semua `-I` path lewat Makefile, tapi IntelliSense
VS Code **tidak membaca Makefile**. Tanpa setup, ia gagal menemukan header
seperti `#include "lwip/init.h"` walau build-nya sukses.

Solusinya: `compile_commands.json` — database yang berisi flag compile **persis**
dari build sungguhan, jadi IntelliSense tidak pernah out-of-sync dengan Makefile.

### File yang di-ignore

Dua file berikut **tidak ikut di repo** (lihat `.gitignore`) karena berisi
path absolut yang spesifik per-mesin — tiap orang generate sendiri:

- `compile_commands.json`
- `.vscode/` (termasuk `c_cpp_properties.json`)

### Cara pakai setelah clone

1. Install generator (sekali saja, butuh Python 3):

   ```bash
   python -m pip install compiledb
   ```

2. Generate database dari build dry-run (ulangi tiap tambah file `.c` baru
   atau ubah `-I` path — **tidak otomatis**):

   ```bash
   make compile_commands
   ```

   Ini menjalankan `make` mode dry-run (`-n`): mencetak perintah compile tanpa
   benar-benar build, lalu compiledb menangkap flag-nya. Pastikan hasilnya
   tidak kosong (`[]`) — harusnya puluhan entri.

3. Buat `.vscode/c_cpp_properties.json` yang menunjuk ke database itu:

   ```json
   {
       "configurations": [
           {
               "name": "MSYS2 Clang64",
               "compileCommands": "${workspaceFolder}/compile_commands.json",
               "includePath": [
                   "${workspaceFolder}/**",
                   "${workspaceFolder}/include",
                   "${workspaceFolder}/third_party/net/lwip/src/include",
                   "${workspaceFolder}/drivers/net/port"
               ],
               "defines": [],
               "compilerPath": "E:/Tools/msys2/clang64/bin/clang.exe",
               "cStandard": "c11",
               "cppStandard": "c++17",
               "intelliSenseMode": "windows-clang-x64",
               "compilerArgs": ["-ffreestanding"]
           }
       ],
       "version": 4
   }
   ```

   > `compileCommands` dipakai untuk file yang ada di database; `includePath`
   > jadi fallback untuk file baru yang belum tertangkap. Sesuaikan
   > `compilerPath` dengan lokasi clang di mesin masing-masing.

4. Command Palette (Ctrl+Shift+P) → **`C/C++: Reset IntelliSense Database`**
   untuk membuang cache lama.
