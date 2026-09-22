#ifndef SPINLOCK_H
#define SPINLOCK_H

// =====================================================================
// test/spinlock.h — MOCK untuk host test (dipakai via -iquote test).
//
// include/spinlock.h memakai instruksi privileged (cli/sti) yang akan
// fault saat dijalankan di ring 3 host. Modul kernel yang diuji di host
// hanya butuh API + tipe yang sama, bukan mutual exclusion sungguhan —
// host test single-threaded, jadi lock bisa no-op.
//
// JANGAN pakai header ini untuk kode kernel.
// =====================================================================

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

static inline uint64_t spinlock_irq_save(void) { return 0; }
static inline void     spinlock_irq_restore(uint64_t flags) { (void)flags; }

static inline void spinlock_lock(spinlock_t *lock)   { lock->locked = 1; }
static inline void spinlock_unlock(spinlock_t *lock) { lock->locked = 0; }

static inline int spinlock_try_lock(spinlock_t *lock) {
    (void)lock;
    return 1;
}

static inline int spinlock_try_lock_irqsave(spinlock_t *lock, uint64_t *flags) {
    (void)lock;
    *flags = 0;
    return 1;
}

static inline uint64_t spinlock_lock_irqsave(spinlock_t *lock) {
    (void)lock;
    return 0;
}

static inline void spinlock_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)lock;
    (void)flags;
}

extern spinlock_t g_kernel_lock;

#endif // SPINLOCK_H
