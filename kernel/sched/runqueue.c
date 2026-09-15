// ============================================================
// kernel/sched/runqueue.c — Per-CPU Run Queues, Kyuzen OS
//
// Each CPU owns a FIFO of READY task ids. A READY task is present in EXACTLY
// ONE run queue; a RUNNING task is in none (tracked by cpu_current_task).
// This single-membership invariant is what prevents a task from being run on
// two CPUs at once: a task can only be scheduled by first dequeuing it under
// the owning queue's lock, which removes it from every queue atomically.
//
// Balancing has two halves:
//   - Placement: create_task enqueues onto the least-loaded CPU (pick_target_cpu).
//   - Work-stealing: an idle CPU pulls a task from the busiest remote queue.
//
// Locking: every queue has its own lock and we never hold two queue locks at
// once (stealing dequeues the victim, then enqueues to self as separate critical
// sections), so the run queues cannot deadlock against each other.
// Lock order terhadap scheduler_lock: scheduler_lock → rq->lock, tidak pernah
// dibalik (schedule_on_cpu memegang scheduler_lock lalu memanggil runq_pop).
// ============================================================

#include "task.h"
#include "spinlock.h"
#include "smp.h"
#include "timer.h"
#include "sched_internal.h"
#include <stddef.h>

#define RUNQ_CAPACITY MAX_TASKS   // Never more than MAX_TASKS tasks system-wide.

typedef struct {
    spinlock_t       lock;
    volatile uint32_t count;      // Advisory load metric; aligned 32-bit read is atomic on x86.
    int              entries[RUNQ_CAPACITY];
} run_queue_t;

static run_queue_t cpu_runqueues[SMP_MAX_CPUS];

// Aging: every AGE_STEP_MS a READY task waits adds +1 to its effective priority,
// capped at AGE_MAX_BONUS, so low-priority tasks cannot starve.
#define AGE_STEP_MS    100
#define AGE_MAX_BONUS  8

// Effective priority = base priority + aging bonus from time spent waiting.
static uint32_t effective_prio(int task_id, uint64_t now_ms) {
    uint32_t base = tasks[task_id].priority;
    uint64_t enq  = tasks[task_id].enqueue_ms;
    uint64_t waited = (now_ms > enq) ? (now_ms - enq) : 0;
    uint32_t bonus = (uint32_t)(waited / AGE_STEP_MS);
    if (bonus > AGE_MAX_BONUS) bonus = AGE_MAX_BONUS;
    return base + bonus;
}

void runq_init(void) {
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        run_queue_t *rq = &cpu_runqueues[i];
        rq->lock.locked = 0;
        rq->count = 0;
    }
}

uint32_t runq_len(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) return 0;
    return cpu_runqueues[cpu_id].count;
}

int runq_push(uint32_t cpu_id, int task_id) {
    run_queue_t *rq = &cpu_runqueues[cpu_id];
    uint64_t flags = spinlock_lock_irqsave(&rq->lock);
    if (rq->count >= RUNQ_CAPACITY) {   // Should never happen; fail safe rather than corrupt.
        spinlock_unlock_irqrestore(&rq->lock, flags);
        return -1;
    }
    tasks[task_id].enqueue_ms = timer_get_ms();   // stamp for aging
    rq->entries[rq->count++] = task_id;
    spinlock_unlock_irqrestore(&rq->lock, flags);
    return 0;
}

// Pop the highest effective-priority task; ties break by longest wait (oldest
// enqueue). O(RUNQ_CAPACITY) scan — trivial at MAX_TASKS=8.
int runq_pop(uint32_t cpu_id) {
    run_queue_t *rq = &cpu_runqueues[cpu_id];
    uint64_t flags = spinlock_lock_irqsave(&rq->lock);
    if (rq->count == 0) {
        spinlock_unlock_irqrestore(&rq->lock, flags);
        return -1;
    }

    uint64_t now = timer_get_ms();
    uint32_t best_i = 0;
    uint32_t best_prio = effective_prio(rq->entries[0], now);
    for (uint32_t i = 1; i < rq->count; i++) {
        uint32_t p = effective_prio(rq->entries[i], now);
        if (p > best_prio ||
            (p == best_prio &&
             tasks[rq->entries[i]].enqueue_ms < tasks[rq->entries[best_i]].enqueue_ms)) {
            best_prio = p;
            best_i = i;
        }
    }

    int task_id = rq->entries[best_i];
    rq->entries[best_i] = rq->entries[--rq->count];   // compact: fill gap with last
    spinlock_unlock_irqrestore(&rq->lock, flags);
    return task_id;
}

// Remove every occurrence of task_id from one CPU's queue. Returns 1 if
// anything was removed. P0 Phase 3: kill purge of READY tasks. Locking:
// takes only rq->lock; the caller must hold scheduler_lock (order
// scheduler_lock -> rq->lock, same as schedule_on_cpu).
int runq_remove(uint32_t cpu_id, int task_id) {
    if (cpu_id >= SMP_MAX_CPUS || task_id < 0) return 0;
    run_queue_t *rq = &cpu_runqueues[cpu_id];
    uint64_t flags = spinlock_lock_irqsave(&rq->lock);
    int removed = 0;
    for (uint32_t i = 0; i < rq->count;) {
        if (rq->entries[i] == task_id) {
            rq->entries[i] = rq->entries[--rq->count];
            removed = 1;
            // do not advance: the swapped-in last entry needs checking too
        } else {
            i++;
        }
    }
    spinlock_unlock_irqrestore(&rq->lock, flags);
    return removed;
}

// Pick the least-loaded online CPU for a newly-ready task. Load = queued tasks
// plus one if the CPU is currently running something. A fully idle CPU wins
// immediately so fresh work lands where it can start without waiting a quantum.
uint32_t pick_target_cpu(void) {
    uint32_t online = smp_online_cpu_count();
    if (online > SMP_MAX_CPUS) online = SMP_MAX_CPUS;
    if (online == 0) return 0;

    uint32_t best_cpu  = 0;
    uint32_t best_load = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < online; i++) {
        if (cpu_current_task[i] < 0 && cpu_runqueues[i].count == 0) {
            return i;   // Idle core: schedule here right away.
        }
        uint32_t load = cpu_runqueues[i].count + (cpu_current_task[i] >= 0 ? 1u : 0u);
        if (load < best_load) {
            best_load = load;
            best_cpu  = i;
        }
    }
    return best_cpu;
}

// Work-stealing: pull one task from the busiest remote run queue. Only steals a
// genuinely waiting task (queue length >= 1); the victim's running task is never
// touched. Returns a task id, or -1 if no remote CPU has waiting work.
int steal_task(uint32_t self_cpu) {
    uint32_t online = smp_online_cpu_count();
    if (online > SMP_MAX_CPUS) online = SMP_MAX_CPUS;

    int      victim = -1;
    uint32_t best   = 0;
    for (uint32_t i = 0; i < online; i++) {
        if (i == self_cpu) continue;
        uint32_t c = cpu_runqueues[i].count;   // Advisory: victim may change before we lock.
        if (c > best) {
            best   = c;
            victim = (int)i;
        }
    }
    if (victim < 0) return -1;

    return runq_pop((uint32_t)victim);   // -1 if the queue emptied out before we locked it.
}
