# Scheduler

KyuzenOS uses a preemptive, symmetric-multiprocessing (SMP) scheduler with one
run queue per CPU, priority scheduling with anti-starvation aging, and work
stealing for load balancing.

## Overview

| Property | Value |
| --- | --- |
| Scheduling policy | Priority with aging; FIFO within a priority |
| Run queues | One per CPU (`RUNQ_CAPACITY` = `MAX_TASKS` = 16) |
| Priorities | `PRIO_LOW` (0) … `PRIO_MAX` (3) |
| Aging | `AGE_STEP_MS` = 100 ms, `AGE_MAX_BONUS` = 8 |
| Preemption quantum | 20 ms (scheduler), timer IRQ at the active refresh rate |
| Timer | PIT (boot CPU), LAPIC timer (APs), vector `0xF0` |
| Max tasks | `MAX_TASKS` = 16 |
| Task stack | `TASK_STACK_SIZE` = 16384 (16 KB) |
| Syscall stack | `SYSCALL_STACK_SIZE` = 16384 (16 KB) per CPU |
| Max CPUs | `SMP_MAX_CPUS` = 16 |

## Module Structure

The scheduler lives in `kernel/sched/`, split by responsibility. This is a
structural split with no behavior change; `include/task.h` is unchanged.

| File | Responsibility | Key symbols |
| --- | --- | --- |
| `core.c` | Context switch, CR3 switch, idle loop, globals | `schedule_on_cpu`, `schedule`, `scheduler_idle_loop`, `smp_current_task_id`, `yield`, `scheduler_remove_task` |
| `runqueue.c` | Per-CPU run queue, placement, work stealing | `runq_push`, `runq_pop`, `runq_remove`, `pick_target_cpu`, `steal_task`, `runq_len` |
| `lifecycle.c` | Task creation, exit, per-CPU stacks | `tasking_init`, `create_task`, `create_task_prio`, `create_user_task`, `task_fork`, `task_exit` |
| `block.c` | Two-phase blocking and the sleep queue | `block_prepare`, `block_park`, `unblock_task`, `task_sleep_ms` |
| `debug.c` | Diagnostic dump | `scheduler_dump` |
| `sched_internal.h` | Internal contract (not included outside `kernel/sched/`) | — |

## Task States

```c
#define TASK_READY    0  // ready to run, waiting in a run queue
#define TASK_RUNNING  1  // currently executing on a CPU
#define TASK_SLEEPING 2  // timed block (sleep queue, wake_at_ms)
#define TASK_DEAD     3  // finished, slot reusable
#define TASK_BLOCKED  4  // untimed block (wait queue / synchronization)
#define TASK_ZOMBIE   5  // exited, awaiting parent waitpid
```

The scheduler only ever selects `TASK_READY` tasks. Zombies and blocked tasks
are never run again.

## Core Invariants

1. **Single run-queue membership.** A `TASK_READY` task is in exactly one run
   queue. A `TASK_RUNNING` task is in no queue; it is tracked by
   `cpu_current_task[]`, which is written only by the owning CPU and read
   remotely only as an advisory hint. This is what prevents a task from
   running on two cores at once: to run a task, a CPU must first pop it from a
   queue (removing it atomically under the queue lock).
2. **Pop-before-requeue ordering.** In `schedule_on_cpu`, the incoming task is
   popped first (securing exclusive ownership), and only then is the outgoing
   task's context saved and pushed back to a local queue.
3. **Lock order.** `scheduler_lock` is always outermost; per-queue locks are
   always inside it. Two queue locks are never held simultaneously — stealing
   pops a victim and pushes to the local queue as two separate critical
   sections.

## Context Switch

There is no manual `switch_task()`. All context switches happen inside the
timer interrupt:

```text
Timer IRQ (PIT on BSP, LAPIC on AP)
    └─→ timer ISR stub (ASM)
            ├─ PUSHA64                  save all registers onto the stack
            ├─→ timer_handler(rsp)      C handler
            │       └─→ schedule_on_cpu(cpu, r)   choose next task
            │               └── return new RSP
            ├─ mov rsp, rax             switch to the next task's stack
            ├─ POPA64                   restore the next task's registers
            └─ IRETQ                    jump to the next task's RIP
```

`schedule_on_cpu` also performs two additional duties on every switch:

- **RSP0 repoint.** TSS.RSP0 is set to the incoming task's kernel stack top
  (or the per-CPU syscall stack for kernel tasks with no private stack). This
  is required for correctness: if two Ring-3 tasks shared one CPU and RSP0 were
  a shared per-CPU stack, their preemption frames would overlap and corrupt
  each other.
- **CR3 switch.** A user task switches to its own PML4; a kernel task switches
  to the kernel PML4.

## Priorities and Aging

Each task has a base priority (`PRIO_LOW`..`PRIO_MAX`). To prevent starvation,
the effective priority increases with waiting time:

```
effective_prio = base + min(waited_ms / AGE_STEP_MS, AGE_MAX_BONUS)
```

`runq_pop` selects the highest effective priority; ties are broken by the
oldest `enqueue_ms`.

## SMP Load Balancing

Load balancing has two halves.

### Placement

`create_task` places a new task on the least-loaded CPU via `pick_target_cpu`:
an idle core wins immediately; otherwise the CPU with the smallest load
(`count + (running ? 1 : 0)`) is chosen. Only that CPU is woken with a single
reschedule IPI. This replaces an earlier "wake all idle cores" pattern
(thundering herd) where every core raced for one task.

### Work stealing

When a CPU's local run queue is empty, `schedule_on_cpu` steals one task from
the busiest remote queue (`steal_task`). A `TASK_RUNNING` task on another CPU
is never touched — only waiting tasks are stolen.

The shell `sched` command (`scheduler_dump`) prints each CPU's run-queue length
(`cpuN=...(q=M)`) to observe balance.

## Blocking and Sleeping

Blocking is two-phase to close the lost-wakeup window:

1. `block_prepare()` — remove the task from any run queue and register it on
   the wait object, under the object's lock.
2. `block_park()` — actually park the task (set state, switch away).

A wakeup arriving between the two phases is not lost because the waker sets a
flag that `block_park` checks. `unblock_task` requeues a blocked task onto a
run queue. Timed sleeps use the sleep queue with `wake_at_ms`; the timer tick
(`sleepq_check_wakeups`) wakes expired sleepers.

See [Synchronization](synchronization.md) for the primitives built on this
mechanism.

## Task Exit

`task_exit` moves stack ownership to a local register and zeroes `stack_base`
before switching to the idle stack, so the exiting task's stack is never freed
while still in use (a use-after-free fix). The slot is reclaimed by the reaper
in `create_task_prio`, which reuses `TASK_DEAD` slots. For the full process
lifecycle — zombies, reaping, and kill — see [Process Model](processes.md).

## Kernel API

```c
void tasking_init(void);                        // register Task 0; call after init_timer()
int  create_task(void (*func)(void), const char *name);
int  create_task_prio(void (*func)(void), const char *name, int prio);
void task_exit(void);                           // never returns
void yield(void);                               // halt until next interrupt
void scheduler_dump(void);                      // diagnostic
```

`yield()` is a power-saving hint: it halts the CPU until the next interrupt.
It is not required for preemption.

## Current Limitations

- **No idle CPU power management** beyond `hlt`.
- `MAX_TASKS` is a fixed compile-time limit (16).
- Some scheduler helpers are currently unused: `current_task` (write-only),
  `schedule()` (no callers), `block_current_task` (internal only).

## Related Documentation

- [Process Model](processes.md) — fork/exec/spawn, wait, kill
- [Synchronization](synchronization.md) — blocking primitives
- [Interrupts & Timers](interrupts.md) — timer sources and SMP bring-up
- [Reference: Constants](../reference/constants.md)
