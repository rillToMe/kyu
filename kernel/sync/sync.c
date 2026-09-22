// kernel/sync/sync.c — Mutex, semaphore, condvar over wait queues (Fase 3).
// Each object's wait-queue lock guards its own state (monitor pattern),
// closing the lost-wakeup window. Lock order: object wq -> scheduler_lock.

#include "sync.h"
#include "task.h"
#include <stddef.h>

// ---------------- Mutex ----------------

void mutex_init(mutex_t *m) {
    if (!m) return;
    wait_queue_init(&m->wq);
    m->owner  = -1;
    m->locked = 0;
}

void mutex_lock(mutex_t *m) {
    uint64_t f = wait_queue_lock(&m->wq);
    while (m->locked) {
        f = wait_block_locked(&m->wq, f);
    }
    m->locked = 1;
    m->owner  = smp_current_task_id();
    wait_queue_unlock(&m->wq, f);
}

int mutex_trylock(mutex_t *m) {
    uint64_t f = wait_queue_lock(&m->wq);
    int ok = !m->locked;
    if (ok) {
        m->locked = 1;
        m->owner  = smp_current_task_id();
    }
    wait_queue_unlock(&m->wq, f);
    return ok;
}

void mutex_unlock(mutex_t *m) {
    uint64_t f = wait_queue_lock(&m->wq);
    m->locked = 0;
    m->owner  = -1;
    wait_wake_one(&m->wq);
    wait_queue_unlock(&m->wq, f);
}

// ---------------- Semaphore ----------------

void semaphore_init(semaphore_t *s, int32_t initial) {
    if (!s) return;
    wait_queue_init(&s->wq);
    s->count = initial;
}

void semaphore_wait(semaphore_t *s) {
    uint64_t f = wait_queue_lock(&s->wq);
    while (s->count <= 0) {
        f = wait_block_locked(&s->wq, f);
    }
    s->count--;
    wait_queue_unlock(&s->wq, f);
}

int semaphore_trywait(semaphore_t *s) {
    uint64_t f = wait_queue_lock(&s->wq);
    int ok = s->count > 0;
    if (ok) s->count--;
    wait_queue_unlock(&s->wq, f);
    return ok;
}

void semaphore_post(semaphore_t *s) {
    uint64_t f = wait_queue_lock(&s->wq);
    s->count++;
    wait_wake_one(&s->wq);
    wait_queue_unlock(&s->wq, f);
}

// ---------------- Condition variable ----------------

void condvar_init(condvar_t *c) {
    if (!c) return;
    wait_queue_init(&c->wq);
}

// Release m and block atomically: the cv lock is held across the mutex release
// so a signaler (which needs the cv lock to wake) cannot slip in before we are
// enqueued. Re-acquire m after waking.
void condvar_wait(condvar_t *c, mutex_t *m) {
    uint64_t f = wait_queue_lock(&c->wq);
    mutex_unlock(m);
    f = wait_block_locked(&c->wq, f);
    wait_queue_unlock(&c->wq, f);
    mutex_lock(m);
}

void condvar_signal(condvar_t *c) {
    uint64_t f = wait_queue_lock(&c->wq);
    wait_wake_one(&c->wq);
    wait_queue_unlock(&c->wq, f);
}

void condvar_broadcast(condvar_t *c) {
    uint64_t f = wait_queue_lock(&c->wq);
    wait_wake_all(&c->wq);
    wait_queue_unlock(&c->wq, f);
}
