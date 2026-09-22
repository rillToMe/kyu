// kernel/net_socket.c — TCP client sockets over lwIP raw API (Fase 6).
//
// NO_SYS=1 + LWIP_SOCKET=0: no BSD sockets, so this builds on tcp_* callbacks.
// lwIP is not SMP-safe and runs from two contexts: socket calls (any CPU) and
// TCP callbacks (BSP timer poll via cb_network). A single net_lock serializes
// ALL lwIP entry. Callbacks fire from inside the locked poll, so they assume
// the lock is already held and never re-acquire it.
//
// CRITICAL RULE: net_lock is never held across sti_hlt. Blocking ops poll in a
// released-lock loop, otherwise the BSP poll could not deliver the very packets
// they wait for -> deadlock.

#include "net_socket.h"
#include "spinlock.h"
#include <stddef.h>

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"

extern uint64_t timer_get_ms(void);

enum { S_FREE = 0, S_ALLOC, S_CONNECTING, S_CONNECTED, S_CLOSED, S_ERR };

typedef struct {
    int              state;
    int              gen;         // generation — dibump saat close; deteksi slot reuse (bug 1.5)
    struct tcp_pcb  *pcb;
    uint8_t          rx[KSOCK_RXBUF];
    uint32_t         rx_head;    // read position
    uint32_t         rx_len;     // bytes available
    int              peer_closed;
} ksock_t;

static ksock_t   socks[KSOCK_MAX];
static spinlock_t net_lock = SPINLOCK_INIT;

void net_lock_acquire(uint64_t *saved) { *saved = spinlock_lock_irqsave(&net_lock); }
void net_lock_release(uint64_t saved)  { spinlock_unlock_irqrestore(&net_lock, saved); }

void ksock_init(void) {
    uint64_t f = spinlock_lock_irqsave(&net_lock);
    for (int i = 0; i < KSOCK_MAX; i++) { socks[i].state = S_FREE; socks[i].pcb = NULL; socks[i].gen = 0; }
    spinlock_unlock_irqrestore(&net_lock, f);
}

// ---- ring buffer (caller holds net_lock) ----
static void rx_push(ksock_t *s, const uint8_t *data, uint32_t n) {
    for (uint32_t i = 0; i < n && s->rx_len < KSOCK_RXBUF; i++) {
        uint32_t tail = (s->rx_head + s->rx_len) % KSOCK_RXBUF;
        s->rx[tail] = data[i];
        s->rx_len++;
    }
}
static uint32_t rx_pop(ksock_t *s, uint8_t *out, uint32_t max) {
    uint32_t n = (max < s->rx_len) ? max : s->rx_len;
    for (uint32_t i = 0; i < n; i++) out[i] = s->rx[(s->rx_head + i) % KSOCK_RXBUF];
    s->rx_head = (s->rx_head + n) % KSOCK_RXBUF;
    s->rx_len -= n;
    return n;
}

// ---- lwIP callbacks: fire from inside the locked poll; lock already held ----
static err_t cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    ksock_t *s = (ksock_t *)arg;
    if (err != ERR_OK) { if (p) pbuf_free(p); return ERR_OK; }
    if (p == NULL) { if (s) s->peer_closed = 1; return ERR_OK; }   // peer FIN
    if (s) {
        for (struct pbuf *q = p; q != NULL; q = q->next)
            rx_push(s, (const uint8_t *)q->payload, q->len);
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t cb_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    ksock_t *s = (ksock_t *)arg;
    if (s) s->state = (err == ERR_OK) ? S_CONNECTED : S_ERR;
    return ERR_OK;
}

static void cb_err(void *arg, err_t err) {
    (void)err;
    ksock_t *s = (ksock_t *)arg;
    if (s) { s->state = S_ERR; s->pcb = NULL; }   // pcb already freed by lwIP
}

// ---- API ----
int ksock_socket(void) {
    uint64_t f = spinlock_lock_irqsave(&net_lock);
    int fd = -1;
    for (int i = 0; i < KSOCK_MAX; i++) if (socks[i].state == S_FREE) { fd = i; break; }
    if (fd < 0) { spinlock_unlock_irqrestore(&net_lock, f); return -1; }

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { spinlock_unlock_irqrestore(&net_lock, f); return -1; }

    ksock_t *s = &socks[fd];
    s->state = S_ALLOC; s->pcb = pcb;
    s->rx_head = 0; s->rx_len = 0; s->peer_closed = 0;
    s->gen = s->gen + 1;                       // tiap alokasi slot = generasi baru
    tcp_arg(pcb, s);
    tcp_recv(pcb, cb_recv);
    tcp_err(pcb, cb_err);
    spinlock_unlock_irqrestore(&net_lock, f);
    return fd;
}

static ksock_t *get(int fd) {
    if (fd < 0 || fd >= KSOCK_MAX) return NULL;
    return &socks[fd];
}

int ksock_connect(int fd, uint32_t ip_be, uint16_t port) {
    __asm__ volatile("sti" ::: "memory");   // syscall entered with IF=0

    uint64_t f = spinlock_lock_irqsave(&net_lock);
    ksock_t *s = get(fd);
    if (!s || s->state != S_ALLOC || !s->pcb) { spinlock_unlock_irqrestore(&net_lock, f); return -1; }
    int gen = s->gen;                            // deteksi slot reuse saat polling (bug 1.5)
    ip_addr_t dst; dst.addr = ip_be;
    s->state = S_CONNECTING;
    err_t err = tcp_connect(s->pcb, &dst, port, cb_connected);
    spinlock_unlock_irqrestore(&net_lock, f);
    if (err != ERR_OK) return -1;

    // Poll for the connected callback with the lock RELEASED (BSP poll must run).
    uint64_t deadline = timer_get_ms() + 5000;
    for (;;) {
        f = spinlock_lock_irqsave(&net_lock);
        ksock_t *s2 = get(fd);
        int st  = (s2 && s2->gen == gen) ? s2->state : S_ERR;
        spinlock_unlock_irqrestore(&net_lock, f);
        if (st == S_CONNECTED) return 0;
        if (st == S_ERR)       return -1;
        if (timer_get_ms() >= deadline) return -1;
        __asm__ volatile("sti\n\t hlt" ::: "memory");
    }
}

int ksock_send(int fd, const void *buf, uint32_t len) {
    if (!buf || len == 0) return 0;
    __asm__ volatile("sti" ::: "memory");

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t sent = 0;
    uint64_t deadline = timer_get_ms() + 5000;
    int gen = -1;

    while (sent < len) {
        uint64_t f = spinlock_lock_irqsave(&net_lock);
        ksock_t *s = get(fd);
        if (s && gen < 0) gen = s->gen;          // tangkap generasi pertama kali
        if (!s || s->gen != gen || s->state != S_CONNECTED || !s->pcb) {
            spinlock_unlock_irqrestore(&net_lock, f);
            return (sent > 0) ? (int)sent : -1;
        }

        uint32_t space = tcp_sndbuf(s->pcb);
        if (space > 0) {
            uint32_t chunk = len - sent;
            if (chunk > space) chunk = space;
            if (chunk > 65535) chunk = 65535;    // tcp_write len adalah u16_t (bug 1.7)
            err_t err = tcp_write(s->pcb, p + sent, (uint16_t)chunk, TCP_WRITE_FLAG_COPY);
            if (err == ERR_OK) { tcp_output(s->pcb); sent += chunk; deadline = timer_get_ms() + 5000; }
            else if (err != ERR_MEM) { spinlock_unlock_irqrestore(&net_lock, f); return (sent > 0) ? (int)sent : -1; }
        }
        spinlock_unlock_irqrestore(&net_lock, f);

        if (sent < len) {
            if (timer_get_ms() >= deadline) return (sent > 0) ? (int)sent : -1;
            __asm__ volatile("sti\n\t hlt" ::: "memory");   // wait for sndbuf / ACK
        }
    }
    return (int)sent;
}

int ksock_recv(int fd, void *buf, uint32_t len) {
    if (!buf || len == 0) return 0;
    __asm__ volatile("sti" ::: "memory");

    uint64_t deadline = timer_get_ms() + 10000;
    int gen = -1;
    for (;;) {
        uint64_t f = spinlock_lock_irqsave(&net_lock);
        ksock_t *s = get(fd);
        if (s && gen < 0) gen = s->gen;
        if (!s || s->gen != gen || (s->state != S_CONNECTED && s->state != S_CLOSED)) {
            spinlock_unlock_irqrestore(&net_lock, f);
            return -1;
        }
        if (s->rx_len > 0) {
            int n = (int)rx_pop(s, (uint8_t *)buf, len);
            spinlock_unlock_irqrestore(&net_lock, f);
            return n;
        }
        int closed = s->peer_closed;
        spinlock_unlock_irqrestore(&net_lock, f);
        if (closed) return 0;                       // peer closed, buffer drained
        if (timer_get_ms() >= deadline) return -1;
        __asm__ volatile("sti\n\t hlt" ::: "memory");
    }
}

int ksock_close(int fd) {
    uint64_t f = spinlock_lock_irqsave(&net_lock);
    ksock_t *s = get(fd);
    if (!s || s->state == S_FREE) { spinlock_unlock_irqrestore(&net_lock, f); return -1; }
    if (s->pcb) {
        tcp_arg(s->pcb, NULL);          // detach: late callbacks must not touch freed slot
        tcp_recv(s->pcb, NULL);
        tcp_err(s->pcb, NULL);
        if (tcp_close(s->pcb) != ERR_OK) tcp_abort(s->pcb);
        s->pcb = NULL;
    }
    s->state = S_FREE;
    s->gen++;   // tandai generasi baru — pemblokir lama (bug 1.5) mendeteksi reuse
    spinlock_unlock_irqrestore(&net_lock, f);
    return 0;
}
