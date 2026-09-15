#ifndef WAIT_H
#define WAIT_H

#include <stdint.h>
#include "spinlock.h"
#include "task.h"

// WAIT QUEUE (Fase 2) — fondasi untuk mutex, semaphore, condvar.
//
// Sebuah wait queue adalah antrian task yang menunggu suatu KONDISI. Kunci
// wait queue (wq->lock) juga berfungsi sebagai kunci yang melindungi kondisi
// milik pemanggil (monitor pattern) — inilah yang menutup celah "lost wakeup":
// mutasi kondisi oleh waker dan pemblokiran diri oleh waiter sama-sama terjadi
// di bawah kunci yang sama.
//
// POLA PEMAKAIAN (waiter):
//   uint64_t f = wait_queue_lock(wq);
//   while (!condition)
//       f = wait_block_locked(wq, f);   // atomik: enqueue+block+unlock+park, lalu re-lock
//   // ... konsumsi kondisi (masih memegang lock) ...
//   wait_queue_unlock(wq, f);
//
// POLA PEMAKAIAN (waker):
//   uint64_t f = wait_queue_lock(wq);
//   condition = true;
//   wait_wake_one(wq);                  // atau wait_wake_all(wq)
//   wait_queue_unlock(wq, f);
//
// Kapasitas = MAX_TASKS: sebuah task hanya bisa menunggu di satu wait queue
// pada satu waktu (saat menunggu ia non-runnable), jadi tidak mungkin ada
// lebih dari MAX_TASKS entri.
//
// Urutan kunci (bebas deadlock): wq->lock → scheduler_lock → run-queue lock.
// Tidak ada jalur yang mengunci ke arah sebaliknya.
#define WAIT_QUEUE_CAPACITY MAX_TASKS

typedef struct wait_queue {
    spinlock_t lock;
    int        waiters[WAIT_QUEUE_CAPACITY];   // FIFO task id yang sedang menunggu
    uint32_t   count;
} wait_queue_t;

#define WAIT_QUEUE_INIT { SPINLOCK_INIT, {0}, 0 }

// Inisialisasi wait queue yang dialokasikan dinamis / di dalam struct lain.
void wait_queue_init(wait_queue_t* wq);

// Ambil/lepas kunci wait queue. Kunci ini juga melindungi kondisi pemanggil.
// Mengembalikan / menerima IRQ flags (interrupt dimatikan selama lock dipegang).
uint64_t wait_queue_lock(wait_queue_t* wq);
void     wait_queue_unlock(wait_queue_t* wq, uint64_t flags);

// Harus dipanggil sambil memegang wait_queue_lock(). Secara atomik: tandai task
// saat ini BLOCKED, masukkan ke antrian, lepaskan kunci, lalu park (tidur tanpa
// busy-wait). Saat dibangunkan, kunci DIAMBIL KEMBALI sebelum kembali, sehingga
// pemanggil bisa mengecek ulang kondisinya di bawah lock. Mengembalikan IRQ
// flags baru untuk dipakai wait_queue_unlock() berikutnya.
uint64_t wait_block_locked(wait_queue_t* wq, uint64_t flags);

// Varian wait_block_locked untuk pemanggil yang memegang referensi ekstra yang
// harus dilepas sebelum terminasi (mis. referensi open-description pipe yang
// menahan objek tetap hidup selama block). Sama persis kecuali observasi kill:
// alih-alih keluar noreturn, ia mengeluarkan diri dari antrian, mengembalikan
// state ke RUNNING, menyimpan 1 ke *killed, dan kembali dengan wq->lock
// DIPEGANG (kontrak sama seperti wake normal) agar pemanggil bisa bersih-bersih
// lalu konvergen ke proc_exit_kill() sendiri. *killed = 0 pada wake normal.
// killed tidak boleh NULL.
uint64_t wait_block_killable(wait_queue_t* wq, uint64_t flags, int* killed);

// Harus dipanggil sambil memegang wait_queue_lock(). Bangunkan satu / semua
// waiter (FIFO). No-op jika antrian kosong.
void wait_wake_one(wait_queue_t* wq);
void wait_wake_all(wait_queue_t* wq);

// Keluarkan task dari antrian tanpa membangunkannya (dipakai saat wait dibatalkan,
// mis. timeout di masa depan). Mengambil wq->lock sendiri.
void wait_abort(wait_queue_t* wq, int task_id);

#endif // WAIT_H
