// Mock spinlock.h khusus host test KyuzenFS (test/kyuzenfs_dir_test.c).
// Spinlock asli (include/spinlock.h) memakai asm x86 (cli/sti, lock xchg) yang
// butuh ring 0 — tidak bisa jalan di host. Test meng-compile kyuzenfs.c dengan
// `-iquote test -iquote include` sehingga `#include "spinlock.h"` menemukan file
// ini dulu. Build kernel TIDAK kena: kernel pakai -Iinclude (bukan -iquote test).
#ifndef TEST_SPINLOCK_H
#define TEST_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

static inline uint64_t spinlock_lock_irqsave(spinlock_t* l) { (void)l; return 0; }
static inline void spinlock_unlock_irqrestore(spinlock_t* l, uint64_t f) { (void)l; (void)f; }

// Jalur panic/shutdown memakai varian try-lock (lihat include/spinlock.h).
// Di host selalu "berhasil" — tidak ada CPU lain yang bisa memegang lock.
static inline int spinlock_try_lock(spinlock_t* l) { (void)l; return 1; }
static inline int spinlock_try_lock_irqsave(spinlock_t* l, uint64_t* f) {
    (void)l;
    if (f) *f = 0;
    return 1;
}

#endif
