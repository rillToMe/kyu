#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "pmm.h"
#include "cred.h"

// ============================================================
// CPU CONTEXT — Full ISR Frame
//
// Layout ini cocok 1-to-1 dengan stack frame yang dibuat
// oleh isr_macro.inc (PUSHA64) saat IRQ0 fires:
//
//   [RSP+  0]  r15   ← RSP (pointer ke registers_t*)
//   [RSP+  8]  r14
//   [RSP+ 16]  r13
//   [RSP+ 24]  r12
//   [RSP+ 32]  r11
//   [RSP+ 40]  r10
//   [RSP+ 48]  r9
//   [RSP+ 56]  r8
//   [RSP+ 64]  rdi
//   [RSP+ 72]  rsi
//   [RSP+ 80]  rbp
//   [RSP+ 88]  rdx
//   [RSP+ 96]  rcx
//   [RSP+104]  rbx
//   [RSP+112]  rax
//   [RSP+120]  int_num
//   [RSP+128]  error_code
//   [RSP+136]  rip    (CPU auto-push)
//   [RSP+144]  cs
//   [RSP+152]  rflags
//   [RSP+160]  rsp
//   [RSP+168]  ss
//
// JANGAN ubah urutan field! Preemptive scheduler bergantung padanya.
// ============================================================
typedef struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_num;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) registers_t;


// ============================================================
// TASK STATE
// ============================================================
#define TASK_READY    0
#define TASK_RUNNING  1
#define TASK_SLEEPING 2   // Timed block: waiting for wake_at_ms (sleep queue)
#define TASK_DEAD     3
#define TASK_BLOCKED  4   // Untimed block: waiting on an object (wait queue / sync)
#define TASK_ZOMBIE   5   // P0 Phase 2: exited, exit_code valid, awaiting parent waitpid

// Phase 10: 16 (dari 8) — desktop + shell + ≥6 app GUI konkuren.
#define MAX_TASKS       16
// 16KB (bukan 8KB): sejak Phase 5A, stack ini juga menjadi RSP0 task ring-3 —
// rantai syscall app (exec → ELF → KFS → ATA) bisa dalam; ukuran disamakan
// dengan per-CPU syscall stack (SYSCALL_STACK_SIZE) yang dulu menampungnya.
#define TASK_STACK_SIZE 16384

// ============================================================
// TASK CONTROL BLOCK (TCB)
// ============================================================
typedef struct task {
    uint32_t id;
    uint64_t rsp;             // RSP ke full ISR frame saat task di-preempt
    uint64_t stack_base;      // Untuk kfree saat task mati
    uint8_t  state;           // TASK_READY / TASK_RUNNING / TASK_DEAD / TASK_SLEEPING / TASK_BLOCKED
    char     name[16];        // Nama task untuk debugging
    phys_addr_t pml4_phys;    // Physical address of this task's PML4 (0 = kernel shared)
    uint32_t cookie;          // Unique per-address-space ID (assigned dynamically by OS)
    uint64_t wake_at_ms;      // TASK_SLEEPING: absolute timer_get_ms() at which to wake (0 = n/a)
    uint8_t  priority;        // Base priority, higher = more important (PRIO_*)
    uint64_t enqueue_ms;      // When this task last entered a run queue (for aging)
    // FIX_005 Tahap 3: state heap user per proses (dikelola kernel/mm/uheap.c).
    // Stack app kini di user range AS (elf.c) — tidak perlu tracking kfree.
    uint64_t uheap_brk;       // vaddr bebas berikutnya (0 = belum pernah alloc)
    void*    uheap_regions;   // linked list uheap_region_t (node di heap kernel)
    uint8_t  kind;            // TASK_KIND_* — asal-usul task (semantik sys_exit)
    cred_t   cred;            // P0 Phase 1: kernel-owned identity; inherit on create
    int32_t  parent_id;       // P0 Phase 2: creator task id, PROC_NO_PARENT (-1) if none
    int32_t  exit_code;       // P0 Phase 2: valid iff state == TASK_ZOMBIE
    int32_t  argc;            // P0 Phase 2: user argv count (debug/proc_info; 0 = kernel/none)
    uint8_t  exit_reason;     // P0 Phase 3: PROC_EXIT_* (valid iff ZOMBIE)
    volatile uint8_t kill_pending; // P0 Phase 3: async kill requested (BLOCKED/
                             // SLEEPING/RUNNING target observes, then proc_exit_kill)
} task_t;

// task_t.kind — Phase 5A: pembeda semantik sys_exit (34).
#define TASK_KIND_KERNEL  0   // task kernel / exec-chain shell: exit → longjmp ke user_shell
#define TASK_KIND_SPAWNED 1   // app ring-3 hasil sys_spawn: exit → terminate task

// Priority levels: higher value = scheduled first. Aging boosts long-waiting
// READY tasks so low-priority work cannot starve indefinitely.
#define PRIO_LOW     0
#define PRIO_NORMAL  1
#define PRIO_HIGH    2
#define PRIO_MAX     3


// ============================================================
// PUBLIC API
// ============================================================

void tasking_init(void);
void create_task(void (*func)(void), const char* name);
void create_task_prio(void (*func)(void), const char* name, uint8_t priority);
void task_exit(void) __attribute__((noreturn));

// Phase 5A: buat task ring-3 BARU untuk app hasil sys_spawn — fake ISR frame
// ber-CS=0x1B/SS=0x23 (iretq langsung ke entry ELF), AS per-proses terpasang
// sebelum task terlihat scheduler (tidak ada race CR3). Semua field di-set
// di dalam scheduler_lock sebelum state=READY.
// P0 Phase 2: argc/argv_uaddr dimuat ke RDI/RSI (SysV); argv array + strings
// sudah di user stack milik AS target. argc>=1 (argv[0]=nama app).
// P0 Phase 5: inherit_fds names the CALLER's fds for the child's 0/1/2
// (NULL = fresh TTY everywhere); installed before the child is runnable.
// Return: task id (>= 0), atau -1 jika gagal (OOM stack / slot penuh / runq penuh).
int create_user_task(uint64_t entry_rip, uint64_t user_rsp,
                     phys_addr_t pml4_phys, uint32_t cookie, const char* name,
                     uint64_t argc, uint64_t argv_uaddr,
                     const int inherit_fds[3]);

// P0 Phase 6B: duplicate a RUNNING user task (fork). Clones the address
// space (full copy), forges a syscall-return trap frame (child resumes
// after the fork trap with rax=0), shares the fd table, clones heap
// metadata + credentials. All-or-nothing: returns child slot id or -1
// with nothing published. parent_rf is the parent's live trap frame.
int task_fork(int parent_id, registers_t* parent_rf);
void scheduler_dump(void);
void scheduler_idle_loop(void) __attribute__((noreturn));

// FIX_001: idle stack permanen per-CPU (dialokasikan di tasking_init, tidak
// pernah di-free). task_exit() pindah ke idle stack SEBELUM stack task DEAD
// di-kfree — menutup race UAF stack antara task_exit dan slot reaper di
// create_task. smp_ap_main juga memakainya untuk meninggalkan stack Limine.
uint64_t task_idle_stack_top(uint32_t cpu_id);
void task_switch_to_idle_stack(uint64_t stack_top) __attribute__((noreturn));
void task_exit_via_idle(uint64_t stack_top, void* old_stack_base) __attribute__((noreturn));
void task_exit_finish_on_idle(void* old_stack_base) __attribute__((noreturn));

// FIX_005 Tahap 1: syscall stack permanen per-CPU (RSP0 untuk transisi
// ring-3 → ring-0). Dialokasikan di tasking_init; AP memakainya di smp_ap_main.
uint64_t task_syscall_stack_top(uint32_t cpu_id);

// Preemptive scheduler — dipanggil dari timer interrupt
registers_t* schedule(registers_t* current_regs);
registers_t* schedule_on_cpu(uint32_t cpu_id, registers_t* current_regs);

// Yield hint: biarkan CPU idle, timer preempt otomatis via IRQ0
void yield(void);

// ============================================================
// VOLUNTARY BLOCKING (Fase 1 — fondasi sleep/wait queue)
//
// block_current_task() menandai task saat ini non-runnable lalu memicu
// context switch via self-IPI reschedule (reschedule_isr.asm melakukan
// `mov rsp, rax`). Task tidak akan dipilih scheduler sampai unblock_task().
//
//   new_state = TASK_SLEEPING (timed, dibangunkan sleep queue) atau
//               TASK_BLOCKED  (untimed, dibangunkan waker eksplisit).
//
// KONTRAK: dipanggil HANYA dari konteks task (bukan dari dalam ISR), dengan
// interrupt enabled. Tidak boleh dipanggil sambil memegang scheduler_lock.
// ============================================================
void block_current_task(uint8_t new_state);

// Split form of block_current_task for wait queues (Fase 2). Lets the caller
// atomically enqueue-then-mark-blocked under its own object lock, closing the
// lost-wakeup window:
//   flags = spinlock_lock_irqsave(&wq->lock);
//   ...enqueue self on wq...
//   int self = block_prepare(TASK_BLOCKED);   // interrupts stay disabled
//   spinlock_unlock_irqrestore(&wq->lock, flags);
//   if (self >= 0) block_park(self);           // park until woken
// block_prepare returns the task id, or -1 if it could not block (no task
// context, or already woken). The caller MUST keep interrupts disabled between
// block_prepare and releasing its object lock.
int  block_prepare(uint8_t new_state);
void block_park(int self);

// Bangunkan task yang sedang BLOCKED/SLEEPING: set READY + enqueue + IPI.
// Aman dipanggil dari ISR maupun konteks task. No-op jika task tidak blocked.
void unblock_task(int task_id);

// Tidur non-busy selama `ms` milidetik. Task masuk sleep queue dan
// dibangunkan oleh timer_handler saat timer_get_ms() >= wake_at_ms.
void task_sleep_ms(uint32_t ms);

// Dipanggil dari timer tick: bangunkan semua task tidur yang wake_at_ms-nya
// sudah lewat. Ringan (O(MAX_TASKS)), aman dari konteks interrupt.
void sleepq_check_wakeups(uint64_t now_ms);

// Per-CPU current task ID (SMP-safe). Returns -1 if idle.
int smp_current_task_id(void);

// P0 Phase 1 — kernel credential accessors (see cred.h for policy).
// Read identity from the task, never from a global. Cheap (one array
// index); safe for future filesystem/syscall authorization paths.
// No-task context (idle/early boot) is treated as kernel/root and is
// documented at the definition site, not silently granted per-task.
uint32_t cred_current_uid(void);
uint32_t cred_current_gid(void);
int      cred_current_is_root(void);
uint32_t cred_task_uid(const task_t* t);
uint32_t cred_task_gid(const task_t* t);

// P0 Phase 2 — process lifecycle (see proc.h; defined in kernel/sched/lifecycle.c
// + kernel/proc/proc.c). Exit never returns. Wait blocks (no polling) via the
// existing wait-queue/block primitives.
void proc_exit(int code) __attribute__((noreturn));
// Reap one exited child. pid==-1 (PROC_WAIT_ANY) = any child.
// status_out: kernel pointer or NULL. Returns child pid, or -1 (no child /
// not our child / bad options). options must be 0.
int  proc_waitpid(int32_t pid, int32_t* status_out, int options);
int32_t proc_getpid(void);    // task id, -1 if idle
int32_t proc_getppid(void);   // parent id, PROC_NO_PARENT if none/idle
// P0 Phase 3 — kill (see proc.h for policy). Requests termination of pid:
// READY targets transition synchronously (never run again); BLOCKED/
// SLEEPING/RUNNING targets are flagged (kill_pending) + woken/kicked and
// converge on proc_exit_kill(). Returns 0 ok, -1 denied/invalid/exited.
int  proc_kill(int32_t pid);
// Kill observation path: same authoritative termination as proc_exit with
// PROC_KILL_EXIT_CODE / PROC_EXIT_KILLED. Noreturn.
void proc_exit_kill(void) __attribute__((noreturn));
// P0-FINAL: preemption kill observation for CPU-bound tasks. Called at the
// top of schedule_on_cpu (timer tick / LAPIC tick / reschedule IPI) — i.e.
// in interrupt context on the preempted task's own kernel stack with no
// locks held. If this CPU's current task is RUNNING user code (CPL3) with
// termination outstanding, converge on proc_exit_kill instead of
// scheduling it again. Noreturn when it fires, cheap no-op otherwise.
// CPL3 matters: user mode holds no kernel locks, so the full exit (which
// takes vfs/kfs/paging locks) cannot self-deadlock. Kernel-mode victims
// keep flowing to the existing syscall-exit observation; BLOCKED/SLEEPING
// victims belong to the unblock machinery and are never diverted here.
void proc_observe_kill_sched(uint32_t cpu_id, registers_t* regs);
// Purge a task id from every run queue (READY only; never touches a
// RUNNING task on any CPU). Caller holds scheduler_lock (order:
// scheduler_lock -> rq lock, same as schedule_on_cpu).
void scheduler_remove_task(int task_id);

extern int    task_count;
extern int    current_task;    // DEPRECATED: only tracks CPU 0. Use smp_current_task_id().
extern task_t tasks[MAX_TASKS];

#endif // TASK_H
