#ifndef SPINLOCK_H
#define SPINLOCK_H
// Host equivalent of the kernel lock, including real exclusion/try-lock.
// Only privileged IRQ masking is stubbed; used by damage race regressions.
#include <stdint.h>
typedef struct { volatile uint32_t locked; } spinlock_t;
#define SPINLOCK_INIT {0}
static inline uint64_t spinlock_irq_save(void) { return 0; }
static inline void spinlock_irq_restore(uint64_t f) { (void)f; }
static inline int spinlock_try_lock(spinlock_t* l) {
    return __atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE) == 0;
}
static inline void spinlock_lock(spinlock_t* l) {
    while (!spinlock_try_lock(l)) __asm__ volatile("pause");
}
static inline void spinlock_unlock(spinlock_t* l) {
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}
static inline uint64_t spinlock_lock_irqsave(spinlock_t* l) {
    spinlock_lock(l); return 0;
}
static inline void spinlock_unlock_irqrestore(spinlock_t* l, uint64_t f) {
    (void)f; spinlock_unlock(l);
}
static inline int spinlock_try_lock_irqsave(spinlock_t* l, uint64_t* f) {
    *f = 0; return spinlock_try_lock(l);
}
#endif
