// ============================================================
// kernel/sched/debug.c — Scheduler Dump, Kyuzen OS
// Dipakai perintah shell untuk melihat state task & per-CPU.
// ============================================================

#include "task.h"
#include "spinlock.h"
#include "smp.h"
#include "sched_internal.h"
#include <stddef.h>

extern void kprint(const char* str);
extern void kprint_num(uint64_t num);

void scheduler_dump(void) {
    task_t task_snapshot[MAX_TASKS];
    int cpu_snapshot[SMP_MAX_CPUS];
    percpu_t per_cpu_snapshot[SMP_MAX_CPUS];
    int snapshot_count;
    int active_count = 0;

    uint64_t flags = spinlock_lock_irqsave(&scheduler_lock);

    snapshot_count = task_count;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_snapshot[i] = tasks[i];
    }
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        cpu_snapshot[i] = cpu_current_task[i];
        percpu_t *cpu = smp_get_cpu((uint32_t)i);
        if (cpu != NULL) {
            per_cpu_snapshot[i] = *cpu;
        }
    }

    spinlock_unlock_irqrestore(&scheduler_lock, flags);

    for (int i = 0; i < snapshot_count && i < MAX_TASKS; i++) {
        if (task_snapshot[i].state != TASK_DEAD) {
            active_count++;
        }
    }

    kprint("[sched] tasks=");
    kprint_num((uint64_t)active_count);
    kprint("/");
    kprint_num((uint64_t)snapshot_count);
    kprint(", online_cpus=");
    kprint_num((uint64_t)smp_online_cpu_count());
    kprint("\n");

    for (int i = 0; i < snapshot_count && i < MAX_TASKS; i++) {
        kprint("  task ");
        kprint_num((uint64_t)task_snapshot[i].id);
        kprint("  ");
        kprint(task_state_name(task_snapshot[i].state));
        kprint("  prio=");
        kprint_num((uint64_t)task_snapshot[i].priority);
        kprint("  ppid=");
        if (task_snapshot[i].parent_id < 0) kprint("-");
        else kprint_num((uint64_t)task_snapshot[i].parent_id);
        kprint("  uid=");
        kprint_num((uint64_t)task_snapshot[i].cred.uid);
        if (task_snapshot[i].state == TASK_ZOMBIE) {
            kprint("  exit=");
            kprint_num((uint64_t)(int64_t)task_snapshot[i].exit_code);
        }
        kprint("  ");
        kprint(task_snapshot[i].name);
        kprint("\n");
    }

    kprint("  cpu map:");
    uint32_t online = smp_online_cpu_count();
    if (online > SMP_MAX_CPUS) online = SMP_MAX_CPUS;
    for (uint32_t i = 0; i < online; i++) {
        kprint(" cpu");
        kprint_num((uint64_t)i);
        kprint("=");
        if (cpu_snapshot[i] >= 0) {
            kprint_num((uint64_t)cpu_snapshot[i]);
        } else {
            kprint("idle");
        }
        kprint("(q=");
        kprint_num((uint64_t)runq_len(i));
        kprint(")");
    }
    kprint("\n");

    for (uint32_t i = 0; i < online; i++) {
        kprint("  cpu");
        kprint_num((uint64_t)i);
        kprint(" lapic=");
        kprint_num((uint64_t)per_cpu_snapshot[i].lapic_id);
        kprint(" sched_ticks=");
        kprint_num((uint64_t)per_cpu_snapshot[i].scheduler_ticks);
        kprint(" idle_ticks=");
        kprint_num((uint64_t)per_cpu_snapshot[i].idle_ticks);
        if (per_cpu_snapshot[i].reschedule_pending) {
            kprint(" resched=pending");
        }
        kprint("\n");
    }
}
