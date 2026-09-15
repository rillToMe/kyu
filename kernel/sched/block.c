// ============================================================
// kernel/sched/block.c — Voluntary Blocking & Sleep Queue, Kyuzen OS
//
// VOLUNTARY BLOCKING (Fase 1) — block_current_task / unblock_task
//
// Model: task menandai dirinya non-runnable (TASK_SLEEPING/TASK_BLOCKED) lalu
// memicu reschedule pass. schedule_on_cpu() menyimpan frame-nya (titik resume)
// tapi TIDAK mengembalikannya ke run queue. Task tetap non-runnable sampai
// unblock_task() membuatnya READY/RUNNING lagi.
//
// KEBENARAN TIDAK BERGANTUNG PADA IPI: timer tick periodik juga menjalankan
// scheduler, jadi IPI yang hilang hanya menambah latensi, bukan menggantung.
//
// State machine (di bawah scheduler_lock):
//   RUNNING --block--> SLEEPING/BLOCKED --unblock--> READY (enqueue) / RUNNING (parked)
// ============================================================

#include "task.h"
#include "spinlock.h"
#include "smp.h"
#include "lapic.h"
#include "timer.h"
#include "sched_internal.h"
#include <stddef.h>

// Volatile read of a task's state — dipakai loop block tanpa memegang lock.
// Byte-aligned read atomic di x86; barrier mencegah compiler meng-cache-nya.
static inline uint8_t task_state_volatile(int id) {
    return *(volatile uint8_t*)&tasks[id].state;
}

// block_prepare — mark the current task non-runnable, WITHOUT parking yet.
// Returns the task id on success, or -1 if there is no schedulable task context
// or the caller is not actually the running task.
//
// Split from block_park so a wait queue can atomically (under ITS lock, with
// interrupts disabled) enqueue the waiter AND mark it blocked before releasing
// the lock — closing the lost-wakeup window. The caller MUST keep interrupts
// disabled between block_prepare and releasing its object lock, otherwise a
// timer tick could deschedule the task while it still holds that lock.
int block_prepare(uint8_t new_state) {
    if (new_state != TASK_SLEEPING && new_state != TASK_BLOCKED) return -1;

    uint32_t cpu_id = smp_current_cpu_index();
    int self = (cpu_id < SMP_MAX_CPUS) ? cpu_current_task[cpu_id] : -1;
    if (self < 0 || self >= task_count) {
        return -1;   // No schedulable task context (idle/boot).
    }

    uint64_t flags = spinlock_lock_irqsave(&scheduler_lock);
    if (tasks[self].state != TASK_RUNNING) {
        // Not the running task (already blocked/woken). Caller should not park.
        spinlock_unlock_irqrestore(&scheduler_lock, flags);
        return -1;
    }
    tasks[self].state = new_state;
    spinlock_unlock_irqrestore(&scheduler_lock, flags);
    return self;
}

// block_park — spin until unblock flips this task back to RUNNING. Each iteration
// forces a reschedule on this CPU: the first pass switches us away (frame saved);
// once descheduled the loop body is frozen and only resumes after we are made
// runnable and re-selected — at which point state is already RUNNING.
// `self` must be the id returned by a preceding block_prepare().
void block_park(int self) {
    if (self < 0 || self >= MAX_TASKS) return;
    uint32_t cpu_id = smp_current_cpu_index();
    while (task_state_volatile(self) != TASK_RUNNING) {
        smp_mark_reschedule(cpu_id);
        lapic_send_reschedule(cpu_id);
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

void block_current_task(uint8_t new_state) {
    int self = block_prepare(new_state);
    if (self < 0) return;   // couldn't block (no context, or already woken)
    block_park(self);
}

void unblock_task(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;

    uint64_t flags = spinlock_lock_irqsave(&scheduler_lock);
    uint8_t st = tasks[task_id].state;
    if (st != TASK_SLEEPING && st != TASK_BLOCKED) {
        // Not blocked (already running/ready/dead). No-op keeps unblock idempotent
        // and race-safe against a concurrent wakeup.
        spinlock_unlock_irqrestore(&scheduler_lock, flags);
        return;
    }

    // Is the task still parked on a CPU (spinning its own block loop, off every
    // run queue)? If so we must NOT enqueue it — that would let a second CPU run
    // the same stack. Just flip it RUNNING and kick that CPU out of hlt.
    int on_cpu = -1;
    for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
        if (cpu_current_task[i] == task_id) { on_cpu = (int)i; break; }
    }

    tasks[task_id].wake_at_ms = 0;

    if (on_cpu >= 0) {
        tasks[task_id].state = TASK_RUNNING;   // parked CPU resumes & exits its loop
        spinlock_unlock_irqrestore(&scheduler_lock, flags);
        smp_mark_reschedule((uint32_t)on_cpu);
        lapic_send_reschedule((uint32_t)on_cpu);
        return;
    }

    // Fully descheduled: mark READY and hand to a run queue. The scheduler sets it
    // RUNNING before resuming its frame, so its block loop sees RUNNING and exits.
    tasks[task_id].state = TASK_READY;
    spinlock_unlock_irqrestore(&scheduler_lock, flags);

    uint32_t target = pick_target_cpu();
    if (runq_push(target, task_id) != 0) {
        // Queue full (MAX_TASKS overflow, unreachable in practice). Re-park as
        // BLOCKED so a later unblock/timer retry can pick it up rather than lose it.
        uint64_t re = spinlock_lock_irqsave(&scheduler_lock);
        if (tasks[task_id].state == TASK_READY) tasks[task_id].state = TASK_BLOCKED;
        spinlock_unlock_irqrestore(&scheduler_lock, re);
        return;
    }
    if (target != smp_current_cpu_index()) {
        smp_mark_reschedule(target);
        lapic_send_reschedule(target);
    }
}

// ============================================================
// SLEEP QUEUE — task_sleep_ms / sleepq_check_wakeups
//
// Implementasi ringan: state per-task (wake_at_ms) + scan O(MAX_TASKS) di timer
// tick. MAX_TASKS kecil (8) sehingga scan lebih murah & sederhana daripada
// linked list terurut, tanpa alokasi.
// ============================================================
void task_sleep_ms(uint32_t ms) {
    if (ms == 0) { yield(); return; }

    uint64_t target = timer_get_ms() + ms;

    uint32_t cpu_id = smp_current_cpu_index();
    int self = (cpu_id < SMP_MAX_CPUS) ? cpu_current_task[cpu_id] : -1;
    if (self < 0 || self >= task_count) {
        // No descheduable task context (early boot / pure idle): fall back to a
        // non-busy halt-wait. Still not a spin — CPU sleeps between interrupts.
        while (timer_get_ms() < target) __asm__ volatile("sti; hlt");
        return;
    }

    uint64_t flags = spinlock_lock_irqsave(&scheduler_lock);
    tasks[self].wake_at_ms = target;
    spinlock_unlock_irqrestore(&scheduler_lock, flags);

    block_current_task(TASK_SLEEPING);
    // P0 Phase 3: kill observed on wake (killer unblocks sleepers; the
    // sleep condition is not re-checked, so the flag is the wake reason).
    if (tasks[self].kill_pending) {
        proc_exit_kill();   // noreturn, authoritative termination
    }
}

void sleepq_check_wakeups(uint64_t now_ms) {
    // Runs in timer-interrupt context. Collect due sleepers under the scheduler
    // lock, then unblock them OUTSIDE the lock (unblock_task takes scheduler_lock
    // and run-queue locks — must not nest here).
    int wake[MAX_TASKS];
    int n = 0;

    uint64_t flags = spinlock_lock_irqsave(&scheduler_lock);
    for (int i = 0; i < MAX_TASKS && i < task_count; i++) {
        if (tasks[i].state == TASK_SLEEPING &&
            tasks[i].wake_at_ms != 0 &&
            now_ms >= tasks[i].wake_at_ms) {
            wake[n++] = i;
        }
    }
    spinlock_unlock_irqrestore(&scheduler_lock, flags);

    for (int i = 0; i < n; i++) {
        unblock_task(wake[i]);   // re-validates state under lock; safe if it raced
    }
}
