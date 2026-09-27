// kernel/net/net_socket.c — TCP client sockets over lwIP raw API.
//
// NO_SYS=1 + LWIP_SOCKET=0: no BSD sockets, so this builds on tcp_* callbacks.
// lwIP is not SMP-safe and runs from two contexts: socket calls (any CPU) and
// TCP callbacks (timer poll via cb_network). A single net_lock serializes
// ALL lwIP entry. Callbacks fire from inside the locked poll, so they assume
// the lock is already held and never re-acquire it.
//
// LOCK ORDER (deadlock-free): net_lock -> scheduler_lock. The only nesting
// is net_wake_waiter() calling unblock_task() (scheduler_lock) while holding
// net_lock. No path ever takes net_lock while holding scheduler_lock or any
// wait-queue lock. proc/vfs cleanup calls net_process_cleanup() OUTSIDE all
// locks (same discipline as vfs_close_all).
//
// BLOCKING: net_lock is never held across a sleep. Blocking ops register
// themselves as the socket's waiter under net_lock, then sleep in
// TASK_SLEEPING via task_sleep_ms() quanta. task_sleep_ms() observes
// kill_pending and converges on proc_exit_kill(), so proc_kill() wakes a
// network-blocked task via its standard unblock_task() path — no
// network-specific scheduler mechanism. Every wait loop also has an
// absolute deadline, so a missed wakeup costs latency, never a hang.

#include "net_socket.h"
#include "spinlock.h"
#include <stddef.h>

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

extern uint64_t timer_get_ms(void);
extern void task_sleep_ms(uint32_t ms);   // BLOCKED/SLEEPING + kill observation
extern void unblock_task(int task_id);    // ISR-safe wake, no-op if not blocked
extern void kprint(const char *str);
extern void kprint_num(uint64_t num);

// Syscalls enter with IF=0 (int 0x80 clears it); re-enable so the timer
// poll that drives lwIP can fire while we block. No-op under the host
// test build (ring-3 `sti` would #GP; the pump mock advances time).
#ifdef KSOCK_HOST_TEST
#define KSOCK_STI() ((void)0)
#else
#define KSOCK_STI() __asm__ volatile("sti" ::: "memory")
#endif

enum { S_FREE = 0, S_ALLOC, S_CONNECTING, S_CONNECTED, S_CLOSED, S_ERR };

typedef struct {
    int              state;
    int              gen;            // >0 while allocated; bumped on free
    int              owner_pid;      // exactly one owner; -1 iff S_FREE
    uint32_t         owner_cookie;   // per-AS id: kills PID-reuse aliasing
    struct tcp_pcb  *pcb;
    struct pbuf     *rx_head;       // queued RX pbufs, owned by the socket
    struct pbuf     *rx_tail;
    uint32_t         rx_bytes;       // total queued payload bytes
    int              peer_closed;    // FIN received (p == NULL)
    int              waiter_tid;     // task sleeping in a blocking op, else -1
} ksock_t;

static ksock_t   socks[KSOCK_MAX];
static spinlock_t net_lock = SPINLOCK_INIT;

// Per-slot callback-context ring: the ONLY values ever handed to lwIP via
// tcp_arg(). Each allocation writes a FRESH, then immutable, snapshot
// {slot, gen, pcb} into ctxs[slot][gen % CTX_RING] and registers that entry.
// A late callback from a previous connection presents an entry whose
// snapshot no longer matches the live socket (generation and/or pcb
// differ) and is dropped before touching socket state. The ring (not one
// entry per slot) is what makes the snapshot immutable: overwriting the
// entry a stale pcb points at would destroy the evidence. Alias is only
// possible after CTX_RING same-slot reallocations AND a recycled pcb
// address — and that last leg is closed by the detach protocol itself:
// callbacks are detached under the same net_lock that all callback
// dispatch holds, so a detached pcb can never fire again.
// This gate is what makes stale-callback corruption impossible even for
// cb_err(), which lwIP calls WITHOUT a pcb parameter (so the pcb-identity
// check used by cb_recv/cb_connected is not available there).
#define CTX_RING 4
typedef struct { int slot; int gen; const void *pcb; } ksock_ctx_t;
static ksock_ctx_t ctxs[KSOCK_MAX][CTX_RING];

void net_lock_acquire(uint64_t *saved) { *saved = spinlock_lock_irqsave(&net_lock); }
void net_lock_release(uint64_t saved)  { spinlock_unlock_irqrestore(&net_lock, saved); }

void ksock_init(void) {
    uint64_t f = spinlock_lock_irqsave(&net_lock);
    for (int i = 0; i < KSOCK_MAX; i++) {
        socks[i].state = S_FREE; socks[i].pcb = NULL; socks[i].gen = 0;
        socks[i].owner_pid = -1; socks[i].owner_cookie = 0;
        socks[i].rx_head = NULL; socks[i].rx_tail = NULL; socks[i].rx_bytes = 0;
        socks[i].peer_closed = 0; socks[i].waiter_tid = -1;
        for (int k = 0; k < CTX_RING; k++) {
            ctxs[i][k].slot = i; ctxs[i][k].gen = 0; ctxs[i][k].pcb = NULL;
        }
    }
    spinlock_unlock_irqrestore(&net_lock, f);
}

// ---- handle helpers (caller holds net_lock unless noted) ----

static int handle_slot(int h) { return h & (KSOCK_MAX - 1); }
static int handle_gen(int h)  { return h >> KSOCK_HANDLE_SHIFT; }

// Resolve + authorize in one step. Returns NULL unless: slot in range,
// generation matches (stale-handle protection), state != FREE, and the
// caller is the recorded (pid, cookie) owner.
static ksock_t *resolve(int h, int caller_pid, uint32_t caller_cookie) {
    if (h <= 0 || caller_pid < 0) return NULL;
    int slot = handle_slot(h);
    if (slot < 0 || slot >= KSOCK_MAX) return NULL;
    ksock_t *s = &socks[slot];
    if (s->state == S_FREE) return NULL;
    if (s->gen != handle_gen(h) || s->gen <= 0) return NULL;
    if (s->owner_pid != caller_pid || s->owner_cookie != caller_cookie)
        return NULL;
    return s;
}

// Distinguish "bad/stale handle" from "owned by someone else" for errors.
static int handle_error(int h, int caller_pid, uint32_t caller_cookie) {
    if (h <= 0 || caller_pid < 0) return KSOCK_EHANDLE;
    int slot = handle_slot(h);
    if (slot < 0 || slot >= KSOCK_MAX) return KSOCK_EHANDLE;
    ksock_t *s = &socks[slot];
    if (s->state == S_FREE || s->gen != handle_gen(h) || s->gen <= 0)
        return KSOCK_EHANDLE;
    if (s->owner_pid != caller_pid || s->owner_cookie != caller_cookie)
        return KSOCK_EOWNER;
    return KSOCK_EHANDLE;
}

// Wake the registered waiter, if any. Callbacks run with net_lock held;
// unblock_task() is ISR-safe and a no-op for non-blocked tasks. Spurious
// or redundant wakes are harmless: every waiter re-checks its condition
// under net_lock and every wait loop has a deadline.
static void net_wake_waiter(ksock_t *s) {
    if (s->waiter_tid >= 0) unblock_task(s->waiter_tid);
}

// ---- RX queue (caller holds net_lock) ----

// Free a whole pbuf chain (pbuf_free releases one pbuf only).
static void pbuf_free_chain(struct pbuf *p) {
    while (p) {
        struct pbuf *nx = p->next;
        p->next = NULL;
        pbuf_free(p);
        p = nx;
    }
}
// Drop the whole RX queue WITHOUT tcp_recved: used only when the owning
// pcb is already gone (cb_err) or the socket is being destroyed. Bytes
// never acked are never owed to userspace; the peer retransmits against
// a dead connection and gets RST. Must never run on a live pcb: live
// queued data leaves only via rx_consume() (acked on delivery) or the
// over-limit drop in cb_recv (freed unacked, peer retransmits).
static void rx_free_all(ksock_t *s) {
    struct pbuf *p = s->rx_head;
    s->rx_head = NULL; s->rx_tail = NULL; s->rx_bytes = 0;
    while (p) {
        struct pbuf *nx = p->next;
        p->next = NULL;
        pbuf_free(p);
        p = nx;
    }
}

// Copy up to max bytes from the queue head into out, freeing fully
// consumed pbufs. Returns bytes copied. Caller then tcp_recved(pcb, n)
// for exactly n — the only place an ACK is produced for queued data.
static uint32_t rx_consume(ksock_t *s, uint8_t *out, uint32_t max) {
    uint32_t n = 0;
    while (s->rx_head && n < max) {
        struct pbuf *p = s->rx_head;
        uint32_t take = p->len;
        if (take > max - n) take = max - n;
        const uint8_t *src = (const uint8_t *)p->payload;
        // Partial consume of a pbuf head: slide payload/len, keep tot_len
        // consistent so a later free is exact. (pbuf_header() would do the
        // same; manual slide avoids depending on header-room guarantees.)
        for (uint32_t i = 0; i < take; i++) out[n + i] = src[i];
        n += take;
        s->rx_bytes -= take;
        if (take == (uint32_t)p->len) {
            s->rx_head = p->next;
            if (!s->rx_head) s->rx_tail = NULL;
            p->next = NULL;
            pbuf_free(p);
        } else {
            p->payload = (void *)(src + take);
            p->len = (uint16_t)(p->len - take);
            p->tot_len -= (uint16_t)take;
        }
    }
    return n;
}

// ---- lwIP callbacks: fire from inside the locked poll; lock already held ----

// Slot-validity gate shared by all callbacks. The arg is a ksock_ctx_t*,
// validated as: pointer inside ctxs[], slot field consistent, generation
// matches the live socket, socket non-FREE. A mismatch means the socket
// was closed and the slot reused (or a detached pcb firing late): the
// event is dropped without touching socket state — it can never corrupt
// the new owner. pbuf ownership on the drop path: freed WITHOUT tcp_recved
// (unacked data is retransmitted by the peer; acking it would lie).
// Only static storage is dereferenced (ctxs[], socks[]) — never freed
// memory — so even a wild/late callback faults at most on the gate.
static ksock_t *cb_slot(void *arg) {
    ksock_ctx_t *c = (ksock_ctx_t *)arg;
    if (!c) return NULL;
    if (c < &ctxs[0][0] || c > &ctxs[KSOCK_MAX - 1][CTX_RING - 1]) return NULL;
    int slot = c->slot;
    if (slot < 0 || slot >= KSOCK_MAX) return NULL;
    if (c < &ctxs[slot][0] || c > &ctxs[slot][CTX_RING - 1]) return NULL;
    ksock_t *s = &socks[slot];
    if (s->state == S_FREE || s->gen <= 0) return NULL;
    if (c->gen != s->gen) return NULL;         // stale generation: previous owner
    if (c->pcb != (const void *)s->pcb) return NULL;  // stale pcb snapshot
    return s;
}

static err_t cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    ksock_t *s = cb_slot(arg);
    if (err != ERR_OK) { if (p) pbuf_free_chain(p); return ERR_OK; }
    if (p == NULL) { if (s) { s->peer_closed = 1; net_wake_waiter(s); } return ERR_OK; }
    if (!s || s->pcb != pcb) { pbuf_free_chain(p); return ERR_OK; }  // stale/detached
    // Bounded queue: keep pbufs UNACKED (no tcp_recved here — the receive
    // window stays shrunk while data is queued = backpressure). Over the
    // limit: free WITHOUT ack so the peer retransmits; never ack-then-drop.
    if (s->rx_bytes + (uint32_t)p->tot_len <= KSOCK_RX_LIMIT) {
        p->next = NULL;
        if (s->rx_tail) s->rx_tail->next = p;
        else s->rx_head = p;
        // Append the whole chain tail: p may itself be a chain.
        struct pbuf *t = p;
        while (t->next) t = t->next;
        s->rx_tail = t;
        s->rx_bytes += (uint32_t)p->tot_len;
    } else {
        pbuf_free_chain(p);   // unacked -> retransmit, no data loss, no false ACK
    }
    net_wake_waiter(s);
    return ERR_OK;
}

static err_t cb_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    ksock_t *s = cb_slot(arg);
    if (!s || s->pcb != pcb) return ERR_OK;   // stale: new owner unaffected
    s->state = (err == ERR_OK) ? S_CONNECTED : S_ERR;
    net_wake_waiter(s);
    return ERR_OK;
}

static void cb_err(void *arg, err_t err) {
    (void)err;
    ksock_t *s = cb_slot(arg);
    if (!s) return;                            // stale: never touch new owner
    // pcb already freed by lwIP. Keep queued RX for draining; drop nothing
    // acked-or-not here (rx bytes were never acked, peer will RST/retry).
    s->state = S_ERR; s->pcb = NULL;
    net_wake_waiter(s);
}

// ---- blocking wait (no lock held) ----

// Register as the socket's waiter, sleep one quantum, unregister.
// Must be called WITHOUT net_lock held (task_sleep_ms blocks).
// Kill path: task_sleep_ms() observes kill_pending and converges on
// proc_exit_kill() (noreturn) — a killed waiter never returns here, and
// its sockets are released by net_process_cleanup() inside proc_do_exit().
static void wait_quantum(int slot, int self) {
    uint64_t f = spinlock_lock_irqsave(&net_lock);
    if (slot >= 0 && slot < KSOCK_MAX && socks[slot].state != S_FREE)
        socks[slot].waiter_tid = self;
    spinlock_unlock_irqrestore(&net_lock, f);
    task_sleep_ms(KSOCK_WAIT_QUANTUM_MS);
    f = spinlock_lock_irqsave(&net_lock);
    if (slot >= 0 && slot < KSOCK_MAX && socks[slot].waiter_tid == self)
        socks[slot].waiter_tid = -1;
    spinlock_unlock_irqrestore(&net_lock, f);
}

// ---- API ----

int ksock_socket(int caller_pid, uint32_t caller_cookie) {
    if (caller_pid < 0) return KSOCK_EOWNER;
    KSOCK_STI();
    uint64_t f = spinlock_lock_irqsave(&net_lock);
    int slot = -1;
    for (int i = 0; i < KSOCK_MAX; i++)
        if (socks[i].state == S_FREE) { slot = i; break; }
    if (slot < 0) { spinlock_unlock_irqrestore(&net_lock, f); return KSOCK_ENOMEM; }

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { spinlock_unlock_irqrestore(&net_lock, f); return KSOCK_ENOMEM; }

    ksock_t *s = &socks[slot];
    s->state = S_ALLOC; s->pcb = pcb;
    s->rx_head = NULL; s->rx_tail = NULL; s->rx_bytes = 0; s->peer_closed = 0;
    s->waiter_tid = -1;
    s->owner_pid = caller_pid; s->owner_cookie = caller_cookie;
    s->gen++;                                 // fresh generation per allocation
    if (s->gen <= 0) s->gen = 1;              // never 0/wrap into invalid
    ctxs[slot][s->gen % CTX_RING].gen = s->gen;   // immutable snapshot...
    ctxs[slot][s->gen % CTX_RING].pcb = (const void *)pcb;  // ...for this conn
    int h = (s->gen << KSOCK_HANDLE_SHIFT) | slot;
    tcp_arg(pcb, &ctxs[slot][s->gen % CTX_RING]);
    tcp_recv(pcb, cb_recv);
    tcp_err(pcb, cb_err);
    spinlock_unlock_irqrestore(&net_lock, f);
    return h;
}

int ksock_connect(int h, uint32_t ip_be, uint16_t port,
                  int caller_pid, uint32_t caller_cookie) {
    KSOCK_STI();
    int self = caller_pid;

    uint64_t f = spinlock_lock_irqsave(&net_lock);
    ksock_t *s = resolve(h, caller_pid, caller_cookie);
    if (!s) {
        int e = handle_error(h, caller_pid, caller_cookie);
        spinlock_unlock_irqrestore(&net_lock, f);
        return e;
    }
    if (s->state != S_ALLOC || !s->pcb) {
        spinlock_unlock_irqrestore(&net_lock, f);
        return KSOCK_EHANDLE;
    }
    int slot = handle_slot(h);
    ip_addr_t dst; dst.addr = ip_be;
    s->state = S_CONNECTING;
    err_t err = tcp_connect(s->pcb, &dst, port, cb_connected);
    if (err != ERR_OK) {
        s->state = S_ERR;
        spinlock_unlock_irqrestore(&net_lock, f);
        return KSOCK_ECONN;
    }
    spinlock_unlock_irqrestore(&net_lock, f);

    // Blocking wait with lock RELEASED (BSP poll must run). Kill-aware via
    // task_sleep_ms; stale-handle safe via generation re-check each slice.
    uint64_t deadline = timer_get_ms() + 5000;
    for (;;) {
        f = spinlock_lock_irqsave(&net_lock);
        ksock_t *s2 = resolve(h, caller_pid, caller_cookie);
        int st = s2 ? s2->state : S_FREE;
        int gone = (s2 == NULL);
        spinlock_unlock_irqrestore(&net_lock, f);
        if (gone) return handle_error(h, caller_pid, caller_cookie);
        if (st == S_CONNECTED) return KSOCK_OK;
        if (st == S_ERR)       return KSOCK_ECONN;
        if ((int64_t)(timer_get_ms() - deadline) >= 0) {
            // Attempt dead: abort the pcb so no late callback can fire into
            // this slot after a future reuse, then dishonor the socket.
            f = spinlock_lock_irqsave(&net_lock);
            ksock_t *s3 = resolve(h, caller_pid, caller_cookie);
            if (s3 && s3->state == S_CONNECTING) {
                if (s3->pcb) {
                    tcp_arg(s3->pcb, NULL);
                    tcp_recv(s3->pcb, NULL);
                    tcp_err(s3->pcb, NULL);
                    tcp_abort(s3->pcb);
                    s3->pcb = NULL;
                }
                s3->state = S_ERR;
            }
            spinlock_unlock_irqrestore(&net_lock, f);
            return KSOCK_ETIMEOUT;
        }
        wait_quantum(slot, self);
    }
}

int ksock_send(int h, const void *buf, uint32_t len,
               int caller_pid, uint32_t caller_cookie) {
    if (!buf || len == 0) return 0;
    KSOCK_STI();
    int self = caller_pid;
    int slot = handle_slot(h);

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t sent = 0;
    uint64_t deadline = timer_get_ms() + 5000;

    while (sent < len) {
        uint64_t f = spinlock_lock_irqsave(&net_lock);
        ksock_t *s = resolve(h, caller_pid, caller_cookie);
        if (!s) {
            int e = handle_error(h, caller_pid, caller_cookie);
            spinlock_unlock_irqrestore(&net_lock, f);
            return (sent > 0) ? (int)sent : e;
        }
        if (s->state != S_CONNECTED || !s->pcb) {
            spinlock_unlock_irqrestore(&net_lock, f);
            return (sent > 0) ? (int)sent : KSOCK_ECLOSED;
        }

        uint32_t space = tcp_sndbuf(s->pcb);
        if (space > 0) {
            uint32_t chunk = len - sent;
            if (chunk > space) chunk = space;
            if (chunk > 65535) chunk = 65535;    // tcp_write len is u16_t
            err_t err = tcp_write(s->pcb, p + sent, (uint16_t)chunk, TCP_WRITE_FLAG_COPY);
            if (err == ERR_OK) { tcp_output(s->pcb); sent += chunk; deadline = timer_get_ms() + 5000; }
            else if (err != ERR_MEM) { spinlock_unlock_irqrestore(&net_lock, f); return (sent > 0) ? (int)sent : KSOCK_ECONN; }
        }
        spinlock_unlock_irqrestore(&net_lock, f);

        if (sent < len) {
            if ((int64_t)(timer_get_ms() - deadline) >= 0)
                return (sent > 0) ? (int)sent : KSOCK_ETIMEOUT;
            wait_quantum(slot, self);   // wait for sndbuf / ACK
        }
    }
    return (int)sent;
}

int ksock_recv(int h, void *buf, uint32_t len,
               int caller_pid, uint32_t caller_cookie) {
    if (!buf || len == 0) return 0;
    KSOCK_STI();
    int self = caller_pid;
    int slot = handle_slot(h);

    uint64_t deadline = timer_get_ms() + 10000;
    for (;;) {
        uint64_t f = spinlock_lock_irqsave(&net_lock);
        ksock_t *s = resolve(h, caller_pid, caller_cookie);
        if (!s) {
            int e = handle_error(h, caller_pid, caller_cookie);
            spinlock_unlock_irqrestore(&net_lock, f);
            return e;
        }
        if (s->state != S_CONNECTED && s->state != S_CLOSED && s->state != S_ERR) {
            spinlock_unlock_irqrestore(&net_lock, f);
            return KSOCK_EHANDLE;
        }
        if (s->rx_bytes > 0) {
            // Consume into the kernel bounce buffer, then ack EXACTLY what
            // was consumed. Both under net_lock (lwIP entry, bounded copy).
            int n = (int)rx_consume(s, (uint8_t *)buf, len);
            // Ack exactly what was consumed, in u16_t pieces (tcp_recved
            // takes u16_t; a 64K recv exceeds it).
            uint32_t acked = 0;
            if (s->pcb && n > 0) {
                while (acked < (uint32_t)n) {
                    uint32_t step = (uint32_t)n - acked;
                    if (step > 60000) step = 60000;
                    tcp_recved(s->pcb, (uint16_t)step);
                    acked += step;
                }
            }
            spinlock_unlock_irqrestore(&net_lock, f);
            return n;
        }
        int closed = s->peer_closed || s->state == S_ERR;
        spinlock_unlock_irqrestore(&net_lock, f);
        if (closed) return 0;                       // FIN drained / conn dead
        if ((int64_t)(timer_get_ms() - deadline) >= 0) return KSOCK_ETIMEOUT;
        wait_quantum(slot, self);
    }
}

// Internal close: caller holds net_lock, ownership already established
// (public close validated it; cleanup passes the recorded owner).
// Exact order: detach PCB callbacks FIRST (no new callback access) ->
// close/abort pcb -> free RX queue -> invalidate handle (FREE + gen bump).
// The socket object is static storage (never freed), so a racing callback
// can only observe a generation mismatch, never freed memory.
static void close_locked(ksock_t *s) {
    if (s->pcb) {
        tcp_arg(s->pcb, NULL);
        tcp_recv(s->pcb, NULL);
        tcp_err(s->pcb, NULL);
        if (tcp_close(s->pcb) != ERR_OK) tcp_abort(s->pcb);
        s->pcb = NULL;
    }
    rx_free_all(s);
    s->state = S_FREE;
    s->owner_pid = -1; s->owner_cookie = 0;
    s->peer_closed = 0; s->waiter_tid = -1;
    s->gen++;
    if (s->gen <= 0) s->gen = 1;
}

int ksock_close(int h, int caller_pid, uint32_t caller_cookie) {
    uint64_t f = spinlock_lock_irqsave(&net_lock);
    ksock_t *s = resolve(h, caller_pid, caller_cookie);
    if (!s) {
        int e = handle_error(h, caller_pid, caller_cookie);
        spinlock_unlock_irqrestore(&net_lock, f);
        return e;
    }
    close_locked(s);
    spinlock_unlock_irqrestore(&net_lock, f);
    return KSOCK_OK;
}

void net_process_cleanup(int pid) {
    if (pid < 0) return;
    uint64_t f = spinlock_lock_irqsave(&net_lock);
    for (int i = 0; i < KSOCK_MAX; i++) {
        if (socks[i].state != S_FREE && socks[i].owner_pid == pid)
            close_locked(&socks[i]);
    }
    spinlock_unlock_irqrestore(&net_lock, f);
}

void net_task_reown(int pid, uint32_t new_cookie) {
    if (pid < 0) return;
    uint64_t f = spinlock_lock_irqsave(&net_lock);
    for (int i = 0; i < KSOCK_MAX; i++) {
        if (socks[i].state != S_FREE && socks[i].owner_pid == pid)
            socks[i].owner_cookie = new_cookie;
    }
    spinlock_unlock_irqrestore(&net_lock, f);
}

void ksock_debug_dump(void) {
    uint64_t f = spinlock_lock_irqsave(&net_lock);
    int active = 0;
    for (int i = 0; i < KSOCK_MAX; i++) {
        if (socks[i].state == S_FREE) continue;
        active++;
        kprint("ksock slot="); kprint_num((uint64_t)i);
        kprint(" gen="); kprint_num((uint64_t)socks[i].gen);
        kprint(" st="); kprint_num((uint64_t)socks[i].state);
        kprint(" owner="); kprint_num((uint64_t)socks[i].owner_pid);
        kprint(" rx="); kprint_num((uint64_t)socks[i].rx_bytes);
        kprint(" pcb="); kprint_num((uint64_t)(socks[i].pcb != NULL));
        kprint("\n");
    }
    kprint("ksock active="); kprint_num((uint64_t)active);
    kprint("/"); kprint_num((uint64_t)KSOCK_MAX);
    kprint("\n");
    spinlock_unlock_irqrestore(&net_lock, f);
}
