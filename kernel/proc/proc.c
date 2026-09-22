// ============================================================
// kernel/proc/proc.c — P0 Phase 2: process exit/wait/argv/list.
//
// One authoritative task/process model (task_t is the truth).
// PID = task slot id. Parent explicit. Zombie until reaped.
// SMP: child exit on any CPU wakes parent on any CPU via proc_wq
// (wait-queue lock -> scheduler_lock order, never reversed).
// Blocking uses existing wait_block_locked/block primitives — no polling,
// no new primitive.
// ============================================================

#include "task.h"
#include "proc.h"
#include "wait.h"
#include "usercopy.h"
#include "spinlock.h"
#include "smp.h"
#include "lapic.h"
#include "paging.h"
#include "vfs.h"
#include "uheap.h"
#include "smap.h"
#include <stddef.h>

// scheduler internals (owned by kernel/sched/; declared here instead of
// including sched_internal.h which is private to kernel/sched/).
extern spinlock_t scheduler_lock;
extern int cpu_current_task[];

extern void* kmalloc(uint32_t size);
extern void  kfree(void* ptr);
extern void* memcpy(void* dest, const void* src, size_t n);
extern void  kwm_destroy_windows_of(int task_id);
extern void  flush_event_queue(int task_id);
extern void  vfs_close_all(int task_id);

// proc wait queue: guards the "child exited" condition (monitor pattern).
// Lock order everywhere: proc_wq.lock -> scheduler_lock.
static wait_queue_t proc_wq;

void proc_init(void) {
    wait_queue_init(&proc_wq);
}

int32_t proc_getpid(void) {
    return (int32_t)smp_current_task_id();
}

int32_t proc_getppid(void) {
    int self = smp_current_task_id();
    if (self < 0 || self >= task_count) return PROC_NO_PARENT;
    return tasks[self].parent_id;
}

// basename: "/apps/foo.elf" -> "foo.elf"; "foo" -> "foo". Always NUL-terminated.
void proc_basename(const char* path, char* out, uint32_t cap) {
    if (!out || cap == 0) return;
    if (!path) { out[0] = '\0'; return; }
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    uint32_t i = 0;
    while (base[i] && i + 1 < cap && i + 1 < PROC_MAX_ARG_LEN) {
        out[i] = base[i];
        i++;
    }
    out[i] = '\0';
}

// Copy argv array + strings from the caller into kernel bounce buffers.
// kargv: [PROC_MAX_ARGC][PROC_MAX_ARG_LEN] provided by caller.
// Returns 0 on success, -1 on any validation failure (bounded, no overflow).
int proc_copy_in_argv(const ucopy_ctx_t* uc, uint64_t u_argv, int argc,
                      char kargv[][PROC_MAX_ARG_LEN], uint32_t* total_out) {
    if (total_out) *total_out = 0;
    if (!proc_argc_valid(argc)) return -1;
    if (u_argv == 0) return -1;
    uint64_t ptr_bytes = (uint64_t)argc * 8ULL;
    if (ptr_bytes > (uint64_t)PROC_MAX_ARGC * 8ULL) return -1;

    uint64_t uptrs[PROC_MAX_ARGC];
    for (int i = 0; i < PROC_MAX_ARGC; i++) uptrs[i] = 0;
    if (copy_from_user(uc, uptrs, u_argv, ptr_bytes) != 0) return -1;

    uint32_t total = 0;
    for (int i = 0; i < argc; i++) {
        if (uptrs[i] == 0) return -1;
        int64_t n = strncpy_from_user(uc, kargv[i], uptrs[i], PROC_MAX_ARG_LEN);
        if (n < 0) return -1;
        // strncpy truncates silently at cap-1; reject non-terminated input:
        // re-check the byte at the cap boundary would need another probe.
        // Instead require the source NUL within cap: probe one more byte.
        // Cheap: total length check + explicit NUL already guaranteed by
        // strncpy (kargv[i][n]=='\0' with n<cap). Over-long strings are
        // truncated here and rejected below by total? No — truncated looks
        // valid. Probe the next source byte: if non-zero, input was too long.
        // (Ring0 bypass: kernel strings are trusted, skip probe.)
        if (uc->from_user) {
            char next = 0;
            uint64_t next_addr = uptrs[i] + (uint64_t)n;
            // If the string exactly filled cap-1, next byte decides.
            if (n == (int64_t)(PROC_MAX_ARG_LEN - 1)) {
                if (copy_from_user(uc, &next, next_addr, 1) != 0) return -1;
                if (next != '\0') return -1;   // over PROC_MAX_ARG_LEN
            }
        } else {
            // Ring0: plain strlen check (no user probe needed).
            uint32_t sl = 0;
            while (sl < PROC_MAX_ARG_LEN && kargv[i][sl]) sl++;
            if (sl >= PROC_MAX_ARG_LEN) return -1;
        }
        uint32_t incl = (uint32_t)n + 1;
        if (!proc_arg_len_valid(incl)) return -1;
        if (total + incl < total) return -1;   // wrap
        total += incl;
        if (!proc_arg_total_valid(total)) return -1;
    }
    if (total_out) *total_out = total;
    return 0;
}

// Build argv on the child user stack. Caller: CR3 == child AS, stack pages
// mapped (elf_load_file did). Writes via SMAP window. Updates *stack_top_inout
// to the new aligned RSP and returns the user argv pointer in *argv_out.
// Returns 0 ok, -1 fail (no partial state: caller destroys the AS on fail).
int proc_build_argv(uint64_t* stack_top_inout, int argc,
                    char kargv[][PROC_MAX_ARG_LEN], uint64_t* argv_out) {
    if (!stack_top_inout || !argv_out) return -1;
    if (!proc_argc_valid(argc)) return -1;
    uint64_t old_top = *stack_top_inout;
    if (old_top == 0) return -1;

    uint32_t total = 0;
    uint32_t lens[PROC_MAX_ARGC];
    for (int i = 0; i < argc; i++) {
        uint32_t l = 0;
        while (l < PROC_MAX_ARG_LEN && kargv[i][l]) l++;
        if (l >= PROC_MAX_ARG_LEN) return -1;   // missing NUL
        lens[i] = l + 1;
        total += l + 1;
        if (!proc_arg_total_valid(total)) return -1;
    }

    uint64_t need = (uint64_t)(argc + 1) * 8ULL + total;
    if (need > PROC_ARG_TOTAL_MAX + 136) return -1;
    uint64_t base = (old_top - need) & ~15ULL;
    if (base >= old_top) return -1;   // wrap
    // USER_STACK_TOP/_SIZE mirror elf.h (0x0C000000 / 256KB).
    const uint64_t USTACK_TOP = 0x0C000000ULL;
    const uint64_t USTACK_SZ  = 256ULL * 1024ULL;
    if (base < USTACK_TOP - USTACK_SZ || old_top > USTACK_TOP) return -1;

    uint64_t argv_u = base;
    uint64_t str_u  = base + (uint64_t)(argc + 1) * 8ULL;

    user_access_begin();
    // strings + argv pointer fixups
    uint64_t cur = str_u;
    for (int i = 0; i < argc; i++) {
        uint64_t* slot = (uint64_t*)(argv_u + (uint64_t)i * 8ULL);
        *slot = cur;
        char* dst = (char*)cur;
        for (uint32_t k = 0; k < lens[i]; k++) dst[k] = kargv[i][k];
        cur += lens[i];
    }
    *(uint64_t*)(argv_u + (uint64_t)argc * 8ULL) = 0;   // argv[argc] == NULL
    user_access_end();

    *stack_top_inout = base;
    *argv_out = argv_u;
    return 0;
}

// Fill a kernel buffer with one proc_info per live slot (READY/RUNNING/
// SLEEPING/BLOCKED/ZOMBIE). Returns count. Caller holds no locks.
int proc_fill_list(proc_info_t* kbuf, int max) {
    if (!kbuf || max <= 0) return 0;
    uint64_t f = spinlock_lock_irqsave(&scheduler_lock);
    int n = 0;
    int lim = (task_count < MAX_TASKS) ? task_count : MAX_TASKS;
    for (int i = 0; i < lim && n < max; i++) {
        uint8_t st = tasks[i].state;
        if (st == TASK_DEAD) continue;
        kbuf[n].pid       = (int32_t)tasks[i].id;
        kbuf[n].ppid      = tasks[i].parent_id;
        kbuf[n].state     = st;
        kbuf[n].exit_reason = tasks[i].exit_reason;
        kbuf[n]._pad1     = 0;
        kbuf[n].uid       = tasks[i].cred.uid;
        kbuf[n].gid       = tasks[i].cred.gid;
        kbuf[n].exit_code = tasks[i].exit_code;
        for (int k = 0; k < 16; k++) kbuf[n].name[k] = tasks[i].name[k];
        n++;
    }
    spinlock_unlock_irqrestore(&scheduler_lock, f);
    return n;
}

// Shared TCB transition for ALL termination (self exit + remote sync kill).
// Reparents children to task 0, then ZOMBIE (live waiter parent) or DEAD,
// detaching stack/AS/heap metadata. Caller holds proc_wq.lock ->
// scheduler_lock. switch_as: 1 when the caller runs ON the dying AS
// (self exit: CR3 must leave it before destroy); 0 for a READY remote
// target (its AS is loaded on no CPU). Returns 1 if transitioned, 0 if
// already ZOMBIE/DEAD (double-exit guard: no state touched).
static int proc_transition_locked(int id, int code, uint8_t reason, int switch_as,
                                  void** old_stack_out, phys_addr_t* pml4_out) {
    if (old_stack_out) *old_stack_out = NULL;
    if (pml4_out) *pml4_out = PHYS_NULL;
    if (id < 0 || id >= task_count) return 0;
    if (tasks[id].state == TASK_ZOMBIE || tasks[id].state == TASK_DEAD) return 0;

    // Reparent children to the kernel reaper placeholder (task 0).
    // Task 0 itself reparents to NO_PARENT (nothing to inherit from it).
    int lim = (task_count < MAX_TASKS) ? task_count : MAX_TASKS;
    for (int i = 0; i < lim; i++) {
        if (tasks[i].state != TASK_DEAD && tasks[i].parent_id == id) {
            tasks[i].parent_id = (id != 0) ? 0 : PROC_NO_PARENT;
        }
    }

    // Zombie only for a live WAITING-capable parent. Task 0 never waits, so
    // its (reparented) children auto-reap — no zombie accumulation. Same for
    // NO_PARENT and already-dead/zombie parents (defensive: the reparent
    // above should have removed those cases already).
    int parent = tasks[id].parent_id;
    int parent_alive = (parent > 0 && parent < task_count &&
                        tasks[parent].state != TASK_DEAD &&
                        tasks[parent].state != TASK_ZOMBIE);
    int make_zombie = parent_alive;

    // FIX_001 ownership: stack freed only after the owner leaves it. Self
    // exit frees via the idle-stack dance; remote sync kill kfrees directly
    // (the target is READY: on no CPU, running nowhere).
    if (old_stack_out) *old_stack_out = (void*)tasks[id].stack_base;
    tasks[id].stack_base = 0;

    if (tasks[id].pml4_phys != PHYS_NULL) {
        if (switch_as) {
            vmm_switch_to_kernel_as();   // lock-free CR3 write, safe under lock
        }
        if (pml4_out) *pml4_out = tasks[id].pml4_phys;
        tasks[id].pml4_phys = PHYS_NULL;
    }
    // FIX_005 Tahap 3: drop user-heap metadata before the slot is visible.
    uheap_reset(&tasks[id]);

    tasks[id].rsp = 0;
    tasks[id].wake_at_ms = 0;
    tasks[id].kill_pending = 0;   // consumed: exactly one termination wins
    if (make_zombie) {
        tasks[id].state = TASK_ZOMBIE;
        tasks[id].exit_code = code;
        tasks[id].exit_reason = reason;
        // keep parent_id/argc/name/cred for waitpid + proc_list
    } else {
        tasks[id].state = TASK_DEAD;
        tasks[id].exit_code = code;
        tasks[id].exit_reason = reason;
        tasks[id].parent_id = PROC_NO_PARENT;
    }
    return 1;
}

// One authoritative termination body. Noreturn.
static void proc_do_exit(int code, uint8_t reason) {
    uint32_t cpu_id = smp_current_cpu_index();
    int self = (cpu_id < SMP_MAX_CPUS) ? cpu_current_task[cpu_id] : -1;
    if (self < 0 || self >= MAX_TASKS) {
        for (;;) __asm__ volatile("sti; hlt");
    }

    // Outside all locks: vfs/event/kwm have their own locks.
    vfs_close_all(self);
    flush_event_queue(self);
    kwm_destroy_windows_of(self);

    void*       old_stack = NULL;
    phys_addr_t dead_pml4  = PHYS_NULL;

    // Monitor pattern: proc_wq guards the exit condition; scheduler_lock
    // guards the TCB. Order wq -> scheduler, same as wait path.
    uint64_t wq_f = wait_queue_lock(&proc_wq);
    uint64_t s_f  = spinlock_lock_irqsave(&scheduler_lock);

    int cur = (cpu_id < SMP_MAX_CPUS) ? cpu_current_task[cpu_id] : -1;
    if (cur != self || self >= task_count) {
        spinlock_unlock_irqrestore(&scheduler_lock, s_f);
        wait_queue_unlock(&proc_wq, wq_f);
        for (;;) __asm__ volatile("sti; hlt");
    }

    if (!proc_transition_locked(self, code, reason, 1, &old_stack, &dead_pml4)) {
        // Already ZOMBIE/DEAD: a racing kill consumed the transition.
        // Nothing left to clean (first winner did it all) — park forever.
        spinlock_unlock_irqrestore(&scheduler_lock, s_f);
        wait_queue_unlock(&proc_wq, wq_f);
        for (;;) __asm__ volatile("sti; hlt");
    }
    if (cpu_id < SMP_MAX_CPUS) cpu_current_task[cpu_id] = -1;

    spinlock_unlock_irqrestore(&scheduler_lock, s_f);
    wait_wake_all(&proc_wq);   // wake any parent in proc_waitpid
    wait_queue_unlock(&proc_wq, wq_f);

    // Outside scheduler_lock (takes paging_lock). Safe on the old stack:
    // ownership is in our register, nobody else can free it.
    if (dead_pml4 != PHYS_NULL) {
        vmm_destroy_address_space(dead_pml4, 1);
    }

    task_exit_via_idle(task_idle_stack_top(cpu_id), old_stack);
    for (;;) __asm__ volatile("sti; hlt");   // unreachable; silences noreturn fallthrough
}

// Terminate the current task with a status. Noreturn.
// Zombie iff a live non-reaper parent exists; otherwise DEAD immediately.
// Orphans are reparented to task 0 (kernel reaper placeholder, auto-reaps).
void proc_exit(int code) {
    proc_do_exit(code, PROC_EXIT_NORMAL);
    for (;;) __asm__ volatile("sti; hlt");
}

// Kill observation path: same authoritative termination with the kill
// convention (PROC_KILL_EXIT_CODE / PROC_EXIT_KILLED). Noreturn.
void proc_exit_kill(void) {
    proc_do_exit(PROC_KILL_EXIT_CODE, PROC_EXIT_KILLED);
    for (;;) __asm__ volatile("sti; hlt");
}

// P0-FINAL: preemption kill observation (see task.h). Runs at the top of
// schedule_on_cpu — interrupt context, on the preempted task's own kernel
// stack, no locks held. Only a RUNNING user-mode (CPL3) task with
// kill_pending set diverts here; everything else returns immediately.
// Safety: user mode holds no kernel locks (full exit cannot self-deadlock;
// all exit locks are irqsave leaf-ordered like the normal self-exit), the
// victim's AS/stack are its own (same as proc_exit self path — never a
// remote free), and proc_do_exit revalidates + single-transitions, so a
// concurrent killer/exit can only win the race cleanly (we then park).
// Stale reads are harmless in both directions: a missed flag retries on
// the next tick (20 ms quantum), a just-set flag is owed death anyway.
void proc_observe_kill_sched(uint32_t cpu_id, registers_t* regs) {
    if (!regs || ((regs->cs & 3) != 3)) return;
    if (cpu_id >= SMP_MAX_CPUS) return;
    int self = cpu_current_task[cpu_id];
    if (self < 0 || self >= MAX_TASKS) return;
    if (tasks[self].state != TASK_RUNNING) return;
    if (tasks[self].kind != TASK_KIND_SPAWNED) return;
    if (!tasks[self].kill_pending) return;
    proc_exit_kill();   // noreturn
}

// Request termination of another task (sys_kill / SYS_KILL).
// Policy: proc_can_kill (parenthood via parent_id, root via cred).
//   READY target .... synchronous: purged from every run queue, then the
//                     shared transition runs in the killer's context (safe:
//                     READY = on no CPU, AS loaded nowhere). Never runs again.
//   BLOCKED/SLEEPING  flag (kill_pending) + unblock_task; the target wakes,
//                     observes the flag in its wait path, self-removes from
//                     its wait queue and converges on proc_exit_kill().
//   RUNNING target .. flag + reschedule IPI to the running CPU; observes at
//                     the next syscall/block boundary, or at the next
//                     preemption via proc_observe_kill_sched (covers pure
//                     compute loops with no syscalls), then proc_exit_kill().
// Self-kill (spawned only) exits directly. Returns 0 ok, -1 denied/invalid/
// already-exited. Double kill is idempotent (flag set / already zombie).
int proc_kill(int32_t pid) {
    if (!proc_pid_in_range(pid, MAX_TASKS)) return -1;
    int self = smp_current_task_id();
    int is_root = cred_current_is_root();

    // Authorize + self-kill fast path under one lock.
    {
        uint64_t f = spinlock_lock_irqsave(&scheduler_lock);
        int ok = 0;
        if (pid < task_count) {
            int alive = (tasks[pid].state != TASK_DEAD &&
                         tasks[pid].state != TASK_ZOMBIE);
            int spawned = (tasks[pid].kind == TASK_KIND_SPAWNED);
            ok = proc_can_kill(is_root, self, tasks[pid].parent_id,
                               pid, spawned, alive);
        }
        int is_self = (pid == self);
        spinlock_unlock_irqrestore(&scheduler_lock, f);
        if (!ok) return -1;
        if (is_self) proc_exit_kill();   // noreturn, own context
    }

    // Decide sync vs async under the lock (revalidates state + policy: a
    // racing exit/reparent between the peek above and here must not slip
    // through).
    uint64_t s_f = spinlock_lock_irqsave(&scheduler_lock);
    if (pid >= task_count) {
        spinlock_unlock_irqrestore(&scheduler_lock, s_f);
        return -1;
    }
    {
        int alive = (tasks[pid].state != TASK_DEAD &&
                     tasks[pid].state != TASK_ZOMBIE);
        int spawned = (tasks[pid].kind == TASK_KIND_SPAWNED);
        if (!proc_can_kill(is_root, self, tasks[pid].parent_id,
                           pid, spawned, alive)) {
            spinlock_unlock_irqrestore(&scheduler_lock, s_f);
            return -1;
        }
    }
    if (tasks[pid].kill_pending) {
        spinlock_unlock_irqrestore(&scheduler_lock, s_f);
        return 0;   // double kill: request already outstanding
    }

    if (tasks[pid].state == TASK_READY) {
        // Synchronous kill. Purge first: once removed from every queue the
        // task is reachable by no CPU (READY = in exactly one queue, on no
        // CPU), so the window below is safe. Cleanup (vfs/event/kwm own
        // locks) must not nest under scheduler_lock: release, clean, relock,
        // revalidate — a concurrent killer can only move READY -> ZOMBIE/
        // DEAD, which we treat as success (cleanup is idempotent).
        scheduler_remove_task(pid);
        spinlock_unlock_irqrestore(&scheduler_lock, s_f);

        // Outside all locks (same order as the self-exit path).
        vfs_close_all(pid);
        flush_event_queue(pid);
        kwm_destroy_windows_of(pid);

        void*       old_stack = NULL;
        phys_addr_t dead_pml4  = PHYS_NULL;
        uint64_t wq_f = wait_queue_lock(&proc_wq);
        s_f  = spinlock_lock_irqsave(&scheduler_lock);
        if (pid >= task_count ||
            tasks[pid].state == TASK_ZOMBIE ||
            tasks[pid].state == TASK_DEAD) {
            spinlock_unlock_irqrestore(&scheduler_lock, s_f);
            wait_queue_unlock(&proc_wq, wq_f);
            return 0;   // racing killer already terminated it
        }
        if (tasks[pid].state != TASK_READY) {
            // Defensive: a READY task cannot leave READY except via a queue
            // pop (impossible: purged) or a racing kill (handled above).
            // Fall back to the async request so the target still converges.
            tasks[pid].kill_pending = 1;
            spinlock_unlock_irqrestore(&scheduler_lock, s_f);
            wait_queue_unlock(&proc_wq, wq_f);
            unblock_task(pid);
            return 0;
        }
        proc_transition_locked(pid, PROC_KILL_EXIT_CODE, PROC_EXIT_KILLED,
                               0, &old_stack, &dead_pml4);
        spinlock_unlock_irqrestore(&scheduler_lock, s_f);
        wait_wake_all(&proc_wq);   // wake any parent in proc_waitpid
        wait_queue_unlock(&proc_wq, wq_f);

        // Remote-safe teardown: stack is freeable directly (owner runs
        // nowhere; no idle-stack dance needed) and the AS is loaded on no
        // CPU. Paging lock is taken outside scheduler_lock, as in proc_exit.
        if (dead_pml4 != PHYS_NULL) {
            vmm_destroy_address_space(dead_pml4, 1);
        }
        if (old_stack != NULL) {
            kfree(old_stack);
        }
        return 0;
    }

    // Async kill: flag it; the target converges on proc_exit_kill() at its
    // next safe boundary (wait path / sleep return / syscall entry/exit).
    tasks[pid].kill_pending = 1;
    spinlock_unlock_irqrestore(&scheduler_lock, s_f);

    unblock_task(pid);   // wake BLOCKED/SLEEPING; no-op for RUNNING/READY
    // Kick the CPU if the target is running there now (correctness does not
    // depend on the IPI — the next timer tick schedules anyway — it only
    // shortens the latency to the next observation point).
    {
        uint32_t me = smp_current_cpu_index();
        for (uint32_t c = 0; c < SMP_MAX_CPUS; c++) {
            if (c != me && cpu_current_task[c] == pid) {
                smp_mark_reschedule(c);
                lapic_send_reschedule(c);
            }
        }
    }
    return 0;
}

// Wait for a child to exit. Blocks (no polling). Returns child pid or -1.
int proc_waitpid(int32_t pid, int32_t* status_out, int options) {
    if (options != 0) return -1;
    int self = smp_current_task_id();
    if (self < 0 || self >= MAX_TASKS) return -1;
    if (pid != PROC_WAIT_ANY && (pid < 0 || pid >= MAX_TASKS)) return -1;

    uint64_t wq_f = wait_queue_lock(&proc_wq);
    for (;;) {
        uint64_t s_f = spinlock_lock_irqsave(&scheduler_lock);

        // Policy: pid>0 must be our child; ANY needs at least one child.
        int have_child = 0;
        int target = -1;
        int lim = (task_count < MAX_TASKS) ? task_count : MAX_TASKS;
        if (pid == PROC_WAIT_ANY) {
            for (int i = 0; i < lim; i++) {
                if (tasks[i].state == TASK_DEAD) continue;
                if (tasks[i].parent_id != self) continue;
                have_child = 1;
                if (tasks[i].state == TASK_ZOMBIE) { target = i; break; }
            }
        } else {
            if (pid < lim && tasks[pid].state != TASK_DEAD &&
                tasks[pid].parent_id == self) {
                have_child = 1;
                if (tasks[pid].state == TASK_ZOMBIE) target = pid;
            } else {
                // No such child (never ours / already reaped / invalid).
                // Distinguish "not our child" from "no children at all":
                // both are -1 to the caller (ECHILD); no blocking.
                for (int i = 0; i < lim; i++) {
                    if (tasks[i].state != TASK_DEAD && tasks[i].parent_id == self) {
                        have_child = 1;
                        break;
                    }
                }
                if (!have_child) {
                    spinlock_unlock_irqrestore(&scheduler_lock, s_f);
                    wait_queue_unlock(&proc_wq, wq_f);
                    return -1;
                }
                // pid names a live slot that is not our child (or a dead
                // slot): reject without blocking.
                spinlock_unlock_irqrestore(&scheduler_lock, s_f);
                wait_queue_unlock(&proc_wq, wq_f);
                return -1;
            }
        }

        if (!have_child) {
            spinlock_unlock_irqrestore(&scheduler_lock, s_f);
            wait_queue_unlock(&proc_wq, wq_f);
            return -1;   // no children at all
        }

        if (target >= 0) {
            // Reap: copy status, then free the slot for reuse.
            int32_t code = tasks[target].exit_code;
            if (status_out) *status_out = code;
            tasks[target].state = TASK_DEAD;
            tasks[target].parent_id = PROC_NO_PARENT;
            tasks[target].rsp = 0;
            tasks[target].argc = 0;
            tasks[target].exit_reason = PROC_EXIT_NORMAL;
            tasks[target].kill_pending = 0;
            spinlock_unlock_irqrestore(&scheduler_lock, s_f);
            wait_queue_unlock(&proc_wq, wq_f);
            return target;
        }

        // No zombie yet: block until a child exit wakes us.
        spinlock_unlock_irqrestore(&scheduler_lock, s_f);
        wq_f = wait_block_locked(&proc_wq, wq_f);
        // loop: re-check under lock (wake-all may be for a sibling)
    }
}
