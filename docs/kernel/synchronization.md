# Synchronization

KyuzenOS provides spinlocks for cross-CPU mutual exclusion and higher-level
blocking primitives (mutex, semaphore, condition variable, events, wait
queues) built on the scheduler's two-phase blocking mechanism.

## Overview

| Primitive | Location | Blocking | Use |
| --- | --- | --- | --- |
| `spinlock_t` | `kernel/sync/spinlock.c` | No (busy-wait) | Short critical sections, cross-CPU |
| `mutex_t` | `kernel/sync/` | Yes | Sleepable mutual exclusion |
| `semaphore_t` | `kernel/sync/` | Yes | Counting resource |
| `condvar_t` | `kernel/sync/` | Yes | Condition wait/notify |
| `wait_queue_t` | `kernel/sync/wait.c` | Yes | Generic blocking |
| Events | `kernel/sync/` | Yes | Event flags |

## Spinlocks

A spinlock is a single atomic word with acquire/release semantics. Because a
spinlock may be taken in interrupt context, the IRQ-safe variant saves and
restores the interrupt flag:

```c
spinlock_lock(&lock);
spinlock_unlock(&lock);

uint64_t flags;
spinlock_lock_irqsave(&lock, &flags);
spinlock_unlock_irqrestore(&lock, flags);

int got = spinlock_try_lock(&lock);        // non-blocking acquire
```

### Rules

- **Never sleep while holding a spinlock.** A spinlock holder must not block,
  allocate from a sleeping allocator, or take a blocking lock.
- **Keep critical sections short.** Spinlocks busy-wait, so long sections
  waste CPU and can cause priority inversion with interrupt handlers.
- **Use `irqsave` variants in interrupt context** and wherever the same lock is
  also taken from an interrupt handler. Otherwise a local interrupt can
  deadlock the CPU against itself.
- **`try_lock` when the owner may be interrupted.** The compositor uses
  `spinlock_try_lock` for `kwm_lock`, because a timer interrupt can interrupt a
  print-triggered flush; spinning on that owner would deadlock.

## Global Lock Order

To prevent deadlock, locks are always acquired in a fixed order and never
reversed:

```text
wq->lock  →  scheduler_lock  →  rq->lock
vfs_lock  →  pipe wq->lock
fs_lock   →  bcache_lock
buffer_lock → gtt_lock → fence_lock → submission
```

Rules derived from this order:

- `scheduler_lock` is always outermost with respect to run-queue locks.
- Two run-queue locks are never held simultaneously.
- `vfs_lock` is always taken before a pipe's wait-queue lock.
- `fs_lock` is always taken before `bcache_lock`.

## Two-Phase Blocking

All blocking primitives use a two-phase protocol that closes the lost-wakeup
window:

1. **`block_prepare()`** — remove the task from any run queue and register it
   on the wait object, under the object's lock.
2. **`block_park()`** — park the task (set `TASK_BLOCKED`, switch away).

A wakeup that arrives between the two phases is not lost: the waker marks the
task, and `block_park` checks that mark before actually parking. If already
marked, it does not park.

`unblock_task(task_id)` requeues a blocked task onto a run queue. It is
idempotent and safe to call while holding the wait-queue lock.

### Wait queues

```c
void wait_block_locked(wait_queue_t *wq, spinlock_t *obj_lock);
void wait_remove(wait_queue_t *wq, int task_id);
void wait_abort(wait_queue_t *wq, int task_id);   // remove + unblock
void unblock_task(int task_id);
```

`wait_abort` must both remove the task from the queue **and** call
`unblock_task`, otherwise a task caught between `block_prepare` and
`block_park` sleeps forever. This was a real defect (a lost-wakeup hang) and
the fix is the reason `wait_abort` exists as a distinct helper.

## Mutex / Semaphore / Condition Variable

These are built on wait queues and the two-phase block. Typical usage:

```c
mutex_lock(&m);
/* critical section — may block */
mutex_unlock(&m);

sem_wait(&s);
sem_post(&s);

condvar_wait(&cv, &m);     // atomically release m and block
condvar_signal(&cv);
condvar_broadcast(&cv);
```

### Notes

- A condition variable always pairs with a mutex; `condvar_wait` releases the
  mutex while parked and reacquires it on wake.
- Spurious wakeups are possible; callers re-check their condition in a loop.
- Wake-all is used on every state change in the pipe implementation, with the
  condition re-checked in a loop, to avoid lost wakeups.

## Sleep

`task_sleep_ms(ms)` puts the current task on the sleep queue with
`wake_at_ms = now + ms`. The timer tick (`sleepq_check_wakeups`) wakes expired
sleepers. A sleep is a timed block; a kill pending during the sleep is checked
on return.

## Interrupt-Context Constraints

Interrupt handlers run with interrupts disabled and may hold no sleeping lock.
Where an interrupt handler and a task share data, the lock is an `irqsave`
spinlock. Notable examples:

- `timer_handler` → `cb_network` acquires `net_lock`. `net_lock` is held only
  briefly and no callback re-acquires it, so there is no reentrancy deadlock;
  this is a documented design risk that was evaluated and judged acceptable.
- `kwm_lock` is taken by mouse-IRQ-driven KWM code; the compositor therefore
  only try-locks it.

## Kernel API Summary

| Function | Purpose |
| --- | --- |
| `spinlock_lock` / `spinlock_unlock` | Basic acquire/release |
| `spinlock_lock_irqsave` / `spinlock_unlock_irqrestore` | IRQ-safe variant |
| `spinlock_try_lock` | Non-blocking acquire |
| `block_prepare` / `block_park` | Two-phase block |
| `unblock_task` | Requeue a blocked task |
| `task_sleep_ms` | Timed sleep |
| `wait_block_locked` / `wait_remove` / `wait_abort` | Wait-queue operations |

## Current Limitations

- **No priority inheritance.** A low-priority task holding a lock can block a
  high-priority task (priority inversion). The `cb_network`-in-IRQ risk is
  accepted rather than mitigated.
- **Spinlocks busy-wait.** There is no ticket/queue fairness guarantee beyond
  the hardware atomic.
- **No reader/writer locks.**

## Related Documentation

- [Scheduler](scheduler.md) — run queues, blocking states
- [Processes](processes.md) — waitpid and kill use the same primitives
- [Filesystem](../filesystem/README.md) — `vfs_lock` and pipe locking
