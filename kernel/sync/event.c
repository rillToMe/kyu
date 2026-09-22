#include <stdint.h>
#include "spinlock.h"
#include "task.h"   // MAX_TASKS

// ============================================================
// KYUZEN OS — Event Queue (Phase 5B: per-task)
// Keyboard/mouse ISR me-push lewat push_event_to() SETELAH KWM
// menentukan task tujuan (routing). Task membaca queue miliknya
// sendiri lewat pop_event() dari syscall 29 — app tidak bisa
// lagi saling mencuri event.
// ============================================================

// --- Tipe Data Event (harus cocok dengan userlib.h) ---
#define EVENT_NONE          0
#define EVENT_KEY_PRESS     1
#define EVENT_MOUSE_MOVE    2
#define EVENT_MOUSE_CLICK   3
#define EVENT_SCROLL        4   // Phase 4: P1 = delta wheel (+1 bawah / -1 atas)
#define EVENT_KEY_RELEASE   5   // Phase 4: key up — P1 ASCII dasar, P2 modifier, P3 scancode
#define EVENT_WIN_CLOSE     6   // Phase 5C (dicadangkan): WM minta app menutup window

typedef struct {
    uint32_t type;
    int32_t  param1;
    int32_t  param2;
    int32_t  param3;
    int32_t  win_id;   // Window tujuan (id slot KWM + 1; 0 = tidak relevan)
} kyuzen_event_t;

// --- Circular Buffer per Task ---
// 64 event × 20 byte × MAX_TASKS — kecil; satu lock global cukup (pola sama
// dengan queue global dulu: push dari IRQ, pop/flush irqsave dari syscall).
#define EVENT_QUEUE_SIZE 64

static kyuzen_event_t task_queues[MAX_TASKS][EVENT_QUEUE_SIZE];
static volatile uint32_t eq_head[MAX_TASKS]; // Produsen (IRQ handler) menulis
static volatile uint32_t eq_tail[MAX_TASKS]; // Konsumen (syscall_handler) membaca
static spinlock_t event_lock = SPINLOCK_INIT;

// Masukkan event ke antrian milik task_id (dipanggil dari IRQ handler,
// setelah KWM menentukan routing). win_id = id slot KWM + 1 (0 = tidak relevan).
void push_event_to(int task_id, uint32_t type, int32_t p1, int32_t p2, int32_t p3, int32_t win_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;
    spinlock_lock(&event_lock);
    uint32_t next_head = (eq_head[task_id] + 1) % EVENT_QUEUE_SIZE;
    if (next_head == eq_tail[task_id]) {
        // Buffer penuh — buang event tertua (overwrite)
        eq_tail[task_id] = (eq_tail[task_id] + 1) % EVENT_QUEUE_SIZE;
    }
    task_queues[task_id][eq_head[task_id]].type   = type;
    task_queues[task_id][eq_head[task_id]].param1 = p1;
    task_queues[task_id][eq_head[task_id]].param2 = p2;
    task_queues[task_id][eq_head[task_id]].param3 = p3;
    task_queues[task_id][eq_head[task_id]].win_id = win_id;
    eq_head[task_id] = next_head;
    spinlock_unlock(&event_lock);
}

// Ambil event dari antrian milik task_id. 1 jika ada event, 0 jika kosong.
int pop_event(int task_id, kyuzen_event_t* out) {
    if (task_id < 0 || task_id >= MAX_TASKS) return 0;
    uint64_t flags = spinlock_lock_irqsave(&event_lock);
    if (eq_head[task_id] == eq_tail[task_id]) {
        spinlock_unlock_irqrestore(&event_lock, flags);
        return 0; // Antrian kosong
    }
    *out = task_queues[task_id][eq_tail[task_id]];
    eq_tail[task_id] = (eq_tail[task_id] + 1) % EVENT_QUEUE_SIZE;
    spinlock_unlock_irqrestore(&event_lock, flags);
    return 1;
}

// Buang semua event yang belum dibaca dari antrian milik task_id.
// Dipanggil pada transisi lifecycle (exec/exit/slot reuse) agar event
// app lama tidak bocor ke app berikutnya.
void flush_event_queue(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;
    uint64_t flags = spinlock_lock_irqsave(&event_lock);
    eq_head[task_id] = 0;
    eq_tail[task_id] = 0;
    spinlock_unlock_irqrestore(&event_lock, flags);
}
