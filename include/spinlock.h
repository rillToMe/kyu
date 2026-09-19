#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

static inline uint64_t spinlock_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void spinlock_irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static inline void spinlock_lock(spinlock_t *lock) {
    for (;;) {
        uint32_t taken = 1;
        __asm__ volatile(
            "lock xchg %0, %1"
            : "+r"(taken), "+m"(lock->locked)
            :
            : "memory"
        );
        if (taken == 0) return;
        while (lock->locked) {
            __asm__ volatile("pause");
        }
    }
}

static inline void spinlock_unlock(spinlock_t *lock) {
    __asm__ volatile("" ::: "memory");
    lock->locked = 0;
}

// Coba ambil lock TANPA menunggu. Return 1 kalau berhasil, 0 kalau sedang
// dipegang CPU lain. Dipakai jalur panic/shutdown: menunggu lock di situ bisa
// berarti self-deadlock (CPU yang fault memang pemegang lock) sehingga sistem
// "freeze" tanpa pernah menampilkan BSOD atau reboot.
static inline int spinlock_try_lock(spinlock_t *lock) {
    uint32_t taken = 1;
    __asm__ volatile(
        "lock xchg %0, %1"
        : "+r"(taken), "+m"(lock->locked)
        :
        : "memory"
    );
    return taken == 0;
}

// Versi irqsave dari try_lock: flags selalu ditulis (perilaku sama seperti
// spinlock_lock_irqsave) sehingga pasangan unlock_irqrestore tetap benar
// walau lock gagal didapat.
static inline int spinlock_try_lock_irqsave(spinlock_t *lock, uint64_t *flags) {
    *flags = spinlock_irq_save();
    if (spinlock_try_lock(lock)) return 1;
    spinlock_irq_restore(*flags);   // gagal: kembalikan status IRQ
    return 0;
}

static inline uint64_t spinlock_lock_irqsave(spinlock_t *lock) {
    uint64_t flags = spinlock_irq_save();
    spinlock_lock(lock);
    return flags;
}

static inline void spinlock_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_unlock(lock);
    spinlock_irq_restore(flags);
}

extern spinlock_t g_kernel_lock;

#endif
