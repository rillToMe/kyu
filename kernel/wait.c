// ============================================================
// kernel/wait.c — Wait Queues, Kyuzen OS (Fase 2)
//
// Wait queue = daftar task yang menunggu sebuah kondisi. Ini fondasi untuk
// mutex, semaphore, dan condition variable (Fase 3).
//
// MASALAH INTI — LOST WAKEUP:
//   Pola naif "cek kondisi; kalau belum siap, block" punya lubang: waker bisa
//   mengubah kondisi + memanggil wake PERSIS di antara cek dan block, sehingga
//   task tidur selamanya menunggu wakeup yang sudah lewat.
//
//   Solusi: lock wait queue juga melindungi kondisi milik caller (pola monitor).
//   "Tandai diri blocked + masuk antrian" terjadi di bawah lock yang SAMA dengan
//   yang dipegang waker saat "ubah kondisi + wake". Karena block_prepare menandai
//   task non-runnable secara atomik sebelum lock dilepas, waker tidak mungkin
//   melewatkannya.
//
// URUTAN LOCK (bebas deadlock): wq->lock → scheduler_lock, di kedua jalur.
//   - Jalur tunggu: wait_block_locked → block_prepare (ambil scheduler_lock).
//   - Jalur wake  : wait_wake_* → unblock_task (ambil scheduler_lock).
// ============================================================

#include "wait.h"
#include "task.h"
#include "smp.h"
#include <stddef.h>

void wait_queue_init(wait_queue_t *wq) {
    if (wq == NULL) return;
    wq->lock.locked = 0;
    wq->count = 0;
    for (int i = 0; i < WAIT_QUEUE_CAPACITY; i++) {
        wq->waiters[i] = -1;
    }
}

uint64_t wait_queue_lock(wait_queue_t *wq) {
    return spinlock_lock_irqsave(&wq->lock);
}

void wait_queue_unlock(wait_queue_t *wq, uint64_t flags) {
    spinlock_unlock_irqrestore(&wq->lock, flags);
}

// Enqueue a task id into the queue. Caller MUST hold wq->lock.
// Returns 0 on success, -1 if the queue is full.
static int wait_enqueue(wait_queue_t *wq, int task_id) {
    if (wq->count >= WAIT_QUEUE_CAPACITY) return -1;
    wq->waiters[wq->count++] = task_id;
    return 0;
}

// Remove and return the task id at the head (FIFO). Caller MUST hold wq->lock.
// Returns -1 if empty.
static int wait_dequeue(wait_queue_t *wq) {
    if (wq->count == 0) return -1;
    int id = wq->waiters[0];
    for (uint32_t i = 1; i < wq->count; i++) {
        wq->waiters[i - 1] = wq->waiters[i];
    }
    wq->count--;
    return id;
}

// Remove a specific task id if present (used when a wait is aborted). Caller
// MUST hold wq->lock. Returns 1 if removed, 0 if not found.
static int wait_remove(wait_queue_t *wq, int task_id) {
    for (uint32_t i = 0; i < wq->count; i++) {
        if (wq->waiters[i] == task_id) {
            for (uint32_t j = i + 1; j < wq->count; j++) {
                wq->waiters[j - 1] = wq->waiters[j];
            }
            wq->count--;
            return 1;
        }
    }
    return 0;
}

// Inti bersama wait_block_locked / wait_block_killable. killed == NULL berarti
// perilaku lama (keluar noreturn saat observasi kill). killed != NULL berarti
// laporkan kill: keluarkan diri dari antrian, kembalikan state ke RUNNING,
// tulis *killed = 1, dan kembali dengan wq->lock DIPEGANG — pemanggil melepas
// referensi ekstranya sendiri lalu memanggil proc_exit_kill().
static uint64_t wait_block_impl(wait_queue_t *wq, uint64_t wq_flags, int *killed) {
    // Mark ourselves blocked BEFORE releasing wq->lock. block_prepare marks the
    // task non-runnable atomically; because we still hold wq->lock (and IRQs are
    // off from wait_queue_lock), no waker can observe-and-skip us — they serialize
    // on this same lock. This is what closes the lost-wakeup window.
    int self = block_prepare(TASK_BLOCKED);
    if (self < 0) {
        // No schedulable context, or a wake already fired before we committed.
        // Return still-locked so the caller re-checks its condition under the lock.
        return wq_flags;
    }

    if (wait_enqueue(wq, self) != 0) {
        // Queue full: undo the block so the task stays runnable rather than
        // parking with no waker able to find it. unblock_task flips it back.
        unblock_task(self);
        return wq_flags;
    }

    // P0 Phase 3: kill requested while/before we committed to this queue.
    // Self-remove (lock held: no other waiter/waker can interleave) and
    // converge on the authoritative exit. Checked BEFORE parking so a kill
    // that lands between the killer's (no-op) unblock and our park cannot
    // strand us: every later interleaving is covered by the killer's
    // unblock flipping us back to RUNNING, which makes block_park return
    // immediately to the post-park check below.
    if (tasks[self].kill_pending) {
        wait_remove(wq, self);
        if (killed) {
            // Masih di CPU ini (belum park): unblock mengembalikan BLOCKED
            // ke RUNNING; lock tetap dipegang, pemanggil bersih-bersih.
            unblock_task(self);
            *killed = 1;
            return wq_flags;
        }
        wait_queue_unlock(wq, wq_flags);
        proc_exit_kill();   // noreturn
    }

    // Release the object lock, THEN park. Releasing restores IRQ state (IF was
    // off since wait_queue_lock), so parking runs with interrupts enabled and the
    // self-IPI reschedule can fire. A waker that runs between unlock and park sets
    // our state to RUNNING; block_park sees that immediately and returns without
    // sleeping — no lost wakeup.
    wait_queue_unlock(wq, wq_flags);
    block_park(self);

    // Re-acquire the lock before returning (monitor contract): the caller's
    // while-loop re-checks its condition under the lock, and the returned flags
    // must reflect the new critical section.
    wq_flags = wait_queue_lock(wq);

    // P0 Phase 3: kill observed on wake. A normal wake that won the race
    // still lands here (condition re-check happens in the caller, which we
    // never reach). Self-remove + authoritative exit: exactly one cleanup,
    // no double wakeup, no stale queue entry pinning a reused slot.
    if (tasks[self].kill_pending) {
        wait_remove(wq, self);   // no-op bila sudah di-dequeue oleh waker
        if (killed) {
            *killed = 1;
            return wq_flags;   // lock dipegang; pemanggil bersih-bersih
        }
        wait_queue_unlock(wq, wq_flags);
        proc_exit_kill();   // noreturn
    }
    return wq_flags;
}

uint64_t wait_block_locked(wait_queue_t *wq, uint64_t wq_flags) {
    return wait_block_impl(wq, wq_flags, NULL);
}

uint64_t wait_block_killable(wait_queue_t *wq, uint64_t wq_flags, int *killed) {
    if (!killed) return wait_block_impl(wq, wq_flags, NULL);
    *killed = 0;
    return wait_block_impl(wq, wq_flags, killed);
}

// Caller MUST hold wq->lock (monitor pattern: same lock that guards the
// condition). unblock_task is called while holding wq->lock — safe because the
// lock order wq->lock → scheduler_lock → run-queue lock is never reversed.
void wait_wake_one(wait_queue_t *wq) {
    int id = wait_dequeue(wq);
    if (id >= 0) unblock_task(id);
}

void wait_wake_all(wait_queue_t *wq) {
    int id;
    while ((id = wait_dequeue(wq)) >= 0) {
        unblock_task(id);
    }
}

void wait_abort(wait_queue_t *wq, int task_id) {
    uint64_t flags = wait_queue_lock(wq);
    wait_remove(wq, task_id);
    // Bug 4.1: tanpa unblock, task yang dibatalkan bisa tidur selamanya.
    // Jika task masih di antara block_prepare (state BLOCKED) dan block_park
    // (belum enqueue / belum park), unblock ini mengembalikannya ke RUNNING;
    // jika sudah park di block_park, unblock ini membangunkannya. Idempotent:
    // unblock_task adalah no-op bila state bukan SLEEPING/BLOCKED. Dipanggil
    // sambil memegang wq->lock (urutan lock wq->lock → scheduler_lock).
    unblock_task(task_id);
    wait_queue_unlock(wq, flags);
}
