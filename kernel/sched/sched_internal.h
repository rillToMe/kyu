#ifndef SCHED_INTERNAL_H
#define SCHED_INTERNAL_H

// State & helper internal scheduler yang dibagi antar file kernel/sched/.
// JANGAN di-include dari luar kernel/sched/ — API publik ada di include/task.h.

#include <stdint.h>
#include "spinlock.h"
#include "smp.h"

// Guards TCB slot allocation, task_count, and per-task metadata teardown.
// NOT taken on the scheduling hot path — see cpu_current_task note in core.c.
extern spinlock_t scheduler_lock;

// Per-CPU "currently running task" id (-1 = idle). Each entry is written only
// by its own CPU (schedule_on_cpu / task_exit / idle loop) plus one-time init,
// so it needs no cross-CPU lock. Remote reads (dump, panic) are advisory.
extern int cpu_current_task[SMP_MAX_CPUS];

// runqueue.c — run queue per-CPU + placement + work-stealing.
// Lock order: scheduler_lock (bila dipegang) SELALU sebelum rq->lock.
int      runq_push(uint32_t cpu_id, int task_id);
int      runq_pop(uint32_t cpu_id);
int      runq_remove(uint32_t cpu_id, int task_id);  // P0 Phase 3: kill purge
uint32_t pick_target_cpu(void);
int      steal_task(uint32_t self_cpu);
uint32_t runq_len(uint32_t cpu_id);
void     runq_init(void);

// lifecycle.c — alokasi idle/syscall stack per-CPU (dipanggil tasking_init).
void sched_stacks_init(void);

// core.c — nama state task untuk debug.c.
const char* task_state_name(uint8_t state);

#endif
