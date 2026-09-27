// netsock_test — host regression for the socket lifecycle phase.
//
// Compiles the REAL kernel/net/net_socket.c against fake lwIP headers
// (tests/host/unit/netstub/) plus scheduler/timer mocks, then drives the
// socket state machine deterministically: allocation, ownership, stale
// handles, PCB-callback safety, process cleanup, kill-wakeup plumbing,
// RX flood (no ack-then-drop), and the normal TCP client path.
//
//   make test-netsock
//
// Conventions: CHECK(cond) fails the named test but runs the rest; the
// binary exits non-zero on any failure.

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- mocks required by net_socket.c (defined BEFORE the include) ----
// NOTE: build with -iquote (not -I) for netstub/include/tests/host/unit so
// that ANGLE includes (<stdlib.h> etc.) resolve to the HOST libc while QUOTE
// includes ("spinlock.h", "net_socket.h", "lwip/*") resolve to the mocks and
// the real kernel sources.

#include "spinlock.h"   // tests/host/unit/spinlock.h mock (no cli/sti)
spinlock_t g_kernel_lock;

static uint64_t test_ms;
static int      test_sleep_count;
static int      test_unblock_count;
static int      test_unblock_last;
static int      test_kill_pending;     // observed by mock task_sleep_ms
static int      test_exited;           // set by mock proc_exit_kill
static int      test_exit_pid;
static jmp_buf  test_jmp;
static int      test_in_jmp;
static void   (*test_pump)(void);      // network-event script, runs per sleep

uint64_t timer_get_ms(void) { return test_ms; }
void unblock_task(int tid) { test_unblock_count++; test_unblock_last = tid; }
void proc_exit_kill(void) {
    // Mirrors kernel/proc/proc.c proc_do_exit() order: sockets first.
    extern void net_process_cleanup(int pid);
    test_exited = 1;
    net_process_cleanup(test_exit_pid);
    if (test_in_jmp) longjmp(test_jmp, 1);
    exit(99);   // must never fall through (noreturn in kernel)
}
void task_sleep_ms(uint32_t ms) {
    test_sleep_count++;
    test_ms += ms;
    if (test_pump) test_pump();
    if (test_kill_pending) proc_exit_kill();   // noreturn in kernel
}
void kprint(const char *s) { (void)s; }
void kprint_num(uint64_t n) { (void)n; }

// ---- fake lwIP implementation (declared by netstub headers) ----

#include "lwip/tcp.h"
#include "lwip/pbuf.h"

#define MAX_PCBS 32
static struct tcp_pcb pcb_pool[MAX_PCBS];
static int pcb_used[MAX_PCBS];
static int pcb_live;
static int pcb_cursor;
static err_t test_connect_err = ERR_OK;
static err_t test_write_err = ERR_OK;
static int   test_close_fails;

struct tcp_pcb *tcp_new(void) {
    // Rotating cursor: consecutive allocations prefer different entries so
    // a closed (TIME-WAIT) pcb and its replacement never alias in tests.
    for (int k = 0; k < MAX_PCBS; k++) {
        int i = (pcb_cursor + k) % MAX_PCBS;
        if (!pcb_used[i]) {
            pcb_used[i] = 1;
            pcb_cursor = (i + 1) % MAX_PCBS;
            memset(&pcb_pool[i], 0, sizeof(pcb_pool[i]));
            pcb_pool[i].sndbuf_space = 8192;
            pcb_live++;
            return &pcb_pool[i];
        }
    }
    return NULL;
}
static void pcb_release(struct tcp_pcb *p) {
    int i = (int)(p - pcb_pool);
    if (i >= 0 && i < MAX_PCBS && pcb_used[i]) { pcb_used[i] = 0; pcb_live--; }
}
err_t tcp_connect(struct tcp_pcb *pcb, const ip_addr_t *addr, uint16_t port,
                  tcp_connected_fn c) {
    (void)addr; (void)port;
    pcb->connected_cb = c;
    return test_connect_err;
}
err_t tcp_write(struct tcp_pcb *pcb, const void *d, uint16_t len, uint8_t f) {
    (void)d; (void)f;
    if (pcb->freed || pcb->closed || pcb->aborted) return ERR_CONN;
    if (test_write_err != ERR_OK) return test_write_err;
    pcb->sent_total += len;
    return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *pcb) { (void)pcb; return ERR_OK; }
uint32_t tcp_sndbuf(struct tcp_pcb *pcb) {
    if (pcb->freed || pcb->closed || pcb->aborted) return 0;
    return pcb->sndbuf_space;
}
void tcp_recved(struct tcp_pcb *pcb, uint16_t len) { pcb->recved_total += len; }
void tcp_arg(struct tcp_pcb *pcb, void *a) { pcb->cb_arg = a; }
void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn r) { pcb->recv_cb = r; }
void tcp_err(struct tcp_pcb *pcb, tcp_err_fn e) { pcb->err_cb = e; }
err_t tcp_close(struct tcp_pcb *pcb) {
    if (test_close_fails) return ERR_MEM;
    pcb->closed = 1; pcb->freed = 1;
    pcb_release(pcb);
    return ERR_OK;
}
void tcp_abort(struct tcp_pcb *pcb) {
    pcb->aborted = 1; pcb->freed = 1;
    pcb_release(pcb);
}
static int pbuf_live;
void pbuf_free(struct pbuf *p) {
    if (!p) return;
    free(p->payload);
    free(p);
    pbuf_live--;
}
// Test helper: one pbuf, payload filled with seq bytes starting at `base`.
static struct pbuf *make_pbuf(uint32_t len, uint32_t base) {
    struct pbuf *p = calloc(1, sizeof(*p));
    uint8_t *pl = malloc(len);
    for (uint32_t i = 0; i < len; i++) pl[i] = (uint8_t)((base + i) & 0xFF);
    p->payload = pl;
    p->len = (uint16_t)len;
    p->tot_len = (uint16_t)len;
    pbuf_live++;
    return p;
}

// ---- the real thing ----
#define KSOCK_HOST_TEST 1
#include "../../../kernel/net/net_socket.c"

// ---- test framework ----

static int failures;
static const char *cur_test;
#define CHECK(c) do { \
    if (!(c)) { printf("FAIL [%s] line %d: %s\n", cur_test, __LINE__, #c); failures++; } \
} while (0)

#define P1 3
#define CK1 101u
#define P2 5
#define CK2 202u

static void reset_world(void) {
    test_ms = 1000000; test_sleep_count = 0;
    test_unblock_count = 0; test_unblock_last = -99;
    test_kill_pending = 0; test_exited = 0; test_exit_pid = -1;
    test_pump = NULL; test_in_jmp = 0;
    test_connect_err = ERR_OK; test_write_err = ERR_OK; test_close_fails = 0;
    memset(pcb_used, 0, sizeof(pcb_used)); pcb_live = 0;
    pbuf_live = 0;
    ksock_init();
}

static int active_slots(void) {
    int n = 0;
    for (int i = 0; i < KSOCK_MAX; i++)
        if (socks[i].state != 0) n++;
    return n;
}

// Pump: complete the pending connect on the most recently created pcb.
static struct tcp_pcb *last_pcb;
static void pump_connect_ok(void) {
    for (int i = MAX_PCBS - 1; i >= 0; i--) {
        if (pcb_used[i] && pcb_pool[i].connected_cb && !pcb_pool[i].freed) {
            last_pcb = &pcb_pool[i];
            pcb_pool[i].connected_cb(pcb_pool[i].cb_arg, &pcb_pool[i], ERR_OK);
            return;
        }
    }
}

// Open + connect one socket for (pid, ck); pump completes the handshake.
static int open_connected(int pid, uint32_t ck) {
    int h = ksock_socket(pid, ck);
    if (h < 0) return h;
    test_pump = pump_connect_ok;
    int rc = ksock_connect(h, 0x01020304u, 80, pid, ck);
    test_pump = NULL;
    if (rc != 0) return -1000 - rc;
    return h;
}

// Pump: capture the in-flight handshake, then close it mid-CONNECTING.
static int mid_close_h = -1;
static tcp_connected_fn mid_cap_conn;
static struct tcp_pcb *mid_cap_pcb;
static void *mid_cap_arg;
static void pump_close_mid(void) {
    test_pump = NULL;   // once: let the wait loop observe the close below
    for (int i = 0; i < MAX_PCBS; i++) {
        if (pcb_used[i] && !pcb_pool[i].freed && pcb_pool[i].connected_cb) {
            mid_cap_conn = pcb_pool[i].connected_cb;
            mid_cap_pcb = &pcb_pool[i];
            mid_cap_arg = pcb_pool[i].cb_arg;
            break;
        }
    }
    ksock_close(mid_close_h, P1, CK1);
}

// ---- Test 1: close/reuse — stale handle must fail ----

static void t_close_reuse(void) {
    reset_world();
    int a = ksock_socket(P1, CK1);
    CHECK(a > 0);
    int slot_a = a & 7;
    CHECK(ksock_close(a, P1, CK1) == 0);
    CHECK(ksock_close(a, P1, CK1) == KSOCK_EHANDLE);   // double close
    int b = ksock_socket(P1, CK1);
    CHECK(b > 0);
    CHECK((b & 7) == slot_a);                          // slot reused...
    CHECK(ksock_send(a, "x", 1, P1, CK1) == KSOCK_EHANDLE);  // ...old handle dead
    CHECK(ksock_recv(a, (void *)0x1, 1, P1, CK1) == KSOCK_EHANDLE);
    CHECK(ksock_close(a, P1, CK1) == KSOCK_EHANDLE);
    CHECK(ksock_close(b, P1, CK1) == 0);               // new handle works
    CHECK(active_slots() == 0);
}

// ---- Test 2/3/4: exit cleanup, repeat, 8-slot exhaustion ----

static void t_exit_cleanup(void) {
    reset_world();
    int h1 = ksock_socket(P1, CK1);
    int h2 = ksock_socket(P1, CK1);
    int h3 = ksock_socket(P1, CK1);
    int o  = ksock_socket(P2, CK2);
    CHECK(active_slots() == 4 && pcb_live == 4);
    net_process_cleanup(P1);                            // proc_do_exit path
    CHECK(active_slots() == 1);
    CHECK(ksock_close(h1, P1, CK1) == KSOCK_EHANDLE);
    CHECK(ksock_close(h2, P1, CK1) == KSOCK_EHANDLE);
    CHECK(ksock_close(h3, P1, CK1) == KSOCK_EHANDLE);
    CHECK(ksock_close(o, P2, CK2) == 0);                // other owner intact
    CHECK(active_slots() == 0 && pcb_live == 0);
    net_process_cleanup(P1);                            // idempotent
    CHECK(active_slots() == 0);
}

static void t_repeated_exit(void) {
    reset_world();
    for (int i = 0; i < 20; i++) {
        int h = ksock_socket(P1, (uint32_t)(1000 + i)); // fresh cookie per life
        CHECK(h > 0);
        net_process_cleanup(P1);
        CHECK(active_slots() == 0);
    }
    int h = ksock_socket(P1, CK1);
    CHECK(h > 0);
    CHECK(ksock_close(h, P1, CK1) == 0);
}

static void t_eight_cycles(void) {
    reset_world();
    for (int round = 0; round < 3; round++) {
        int hs[KSOCK_MAX];
        for (int i = 0; i < KSOCK_MAX; i++) {
            hs[i] = ksock_socket(P1, CK1);
            CHECK(hs[i] > 0);
        }
        CHECK(ksock_socket(P1, CK1) == KSOCK_ENOMEM);   // table full, honest err
        net_process_cleanup(P1);                        // process exits
        CHECK(active_slots() == 0);
    }
}

// ---- Test 5: cross-process access denied ----

static void t_cross_process(void) {
    reset_world();
    int h = open_connected(P1, CK1);
    CHECK(h > 0);
    CHECK(ksock_send(h, "x", 1, P2, CK2) == KSOCK_EOWNER);
    char tmp[8];
    CHECK(ksock_recv(h, tmp, sizeof(tmp), P2, CK2) == KSOCK_EOWNER);
    CHECK(ksock_connect(h, 0, 0, P2, CK2) == KSOCK_EOWNER);
    CHECK(ksock_close(h, P2, CK2) == KSOCK_EOWNER);
    CHECK(ksock_close(h, P1, 999u) == KSOCK_EOWNER);    // right pid, wrong cookie
    // Owner still fully functional:
    CHECK(ksock_close(h, P1, CK1) == 0);
    CHECK(ksock_socket(-1, 0) == KSOCK_EOWNER);         // no task context
    CHECK(ksock_close(0, P1, CK1) == KSOCK_EHANDLE);
    CHECK(ksock_close(-5, P1, CK1) == KSOCK_EHANDLE);
}

// ---- Test 6: stale PCB callbacks cannot corrupt the reused slot ----

static void t_stale_pcb(void) {
    reset_world();
    int a = open_connected(P1, CK1);
    CHECK(a > 0);
    // Capture the live pcb + its callback registrations, then close and
    // reuse the slot. The captured fns model a TIME-WAIT/retransmit pcb
    // firing after the slot changed owners.
    struct tcp_pcb *old_pcb = last_pcb;
    tcp_recv_fn old_recv = old_pcb->recv_cb;
    tcp_err_fn old_err = old_pcb->err_cb;
    void *old_arg = old_pcb->cb_arg;
    int slot = a & 7;
    CHECK(ksock_close(a, P1, CK1) == 0);
    int b = open_connected(P1, CK1);
    CHECK(b > 0 && (b & 7) == slot && b != a);
    struct tcp_pcb *new_pcb = last_pcb;
    CHECK(new_pcb != old_pcb);

    // Late callbacks from the OLD connection must be ignored: the generation
    // gate drops them before they touch the new owner's state/rx/pcb.
    struct pbuf *p = make_pbuf(100, 0);
    int before_live = pbuf_live;
    old_recv(old_arg, old_pcb, p, ERR_OK);
    CHECK(pbuf_live == before_live - 1);               // dropped pbuf freed
    old_err(old_arg, ERR_RST);
    // New socket untouched: still connected, no RX, pcb intact.
    CHECK(socks[slot].state == 3 /*S_CONNECTED*/);
    CHECK(socks[slot].pcb == new_pcb);
    CHECK(socks[slot].rx_bytes == 0);
    // Detached pcb firing with NULL arg (post-close tcp_close path): safe.
    struct pbuf *p2 = make_pbuf(10, 0);
    before_live = pbuf_live;
    old_recv(NULL, old_pcb, p2, ERR_OK);
    CHECK(pbuf_live == before_live - 1);
    CHECK(ksock_close(b, P1, CK1) == 0);

    // Close DURING connect: the wait loop must observe the close (not hang),
    // and a late connected-callback must not flip the reused slot.
    int c = ksock_socket(P1, CK1);
    CHECK(c > 0 && (c & 7) == slot);
    test_pump = pump_close_mid;
    mid_close_h = c;
    mid_cap_conn = NULL; mid_cap_pcb = NULL; mid_cap_arg = NULL;
    CHECK(ksock_connect(c, 0x01020304u, 80, P1, CK1) == KSOCK_EHANDLE);
    test_pump = NULL;
    CHECK(mid_cap_conn != NULL);   // pump captured the live handshake
    mid_cap_conn(mid_cap_arg, mid_cap_pcb, ERR_OK);   // stale: must be ignored
    int d = ksock_socket(P1, CK1);
    CHECK(d > 0 && (d & 7) == slot && d != c);
    CHECK(socks[slot].state == 1 /*S_ALLOC*/);          // not CONNECTED
    test_pump = pump_connect_ok;
    CHECK(ksock_connect(d, 0x01020304u, 80, P1, CK1) == 0);
    test_pump = NULL;
    CHECK(ksock_close(d, P1, CK1) == 0);
    CHECK(active_slots() == 0 && pbuf_live == 0);
}

// ---- Test 7: kill while blocked in recv ----

static int kill_pump_fired;
static void kill_pump(void) {
    if (kill_pump_fired) return;
    kill_pump_fired = 1;
    // Waiter must be registered under net_lock before parking.
    CHECK(socks[0].waiter_tid == P1 || socks[1].waiter_tid == P1 ||
          socks[2].waiter_tid == P1 || socks[3].waiter_tid == P1 ||
          socks[4].waiter_tid == P1 || socks[5].waiter_tid == P1 ||
          socks[6].waiter_tid == P1 || socks[7].waiter_tid == P1);
    // Killer path: wake BLOCKED/SLEEPING + flag (mirrors proc_kill).
    unblock_task(P1);
    test_kill_pending = 1;
}

static void t_kill_while_waiting(void) {
    reset_world();
    int h = open_connected(P1, CK1);
    CHECK(h > 0);
    kill_pump_fired = 0;
    test_exit_pid = P1;
    test_pump = kill_pump;
    char tmp[64];
    test_in_jmp = 1;
    if (setjmp(test_jmp) == 0) {
        (void)ksock_recv(h, tmp, sizeof(tmp), P1, CK1);  // must not return
        CHECK(0);   // unreachable: kill converges noreturn
    } else {
        CHECK(test_exited == 1);              // proc_exit_kill ran
        CHECK(active_slots() == 0);           // sockets released on exit
        CHECK(pcb_live == 0);
        CHECK(test_unblock_count >= 2);       // waiter wake + killer wake
        CHECK(ksock_close(h, P1, CK1) == KSOCK_EHANDLE);  // handle stale now
    }
    test_in_jmp = 0;
    test_pump = NULL;
}

// ---- Test 7b: early wake on data (no timeout burn) ----

static int data_pump_n;
static void data_pump(void) {
    if (++data_pump_n < 2) return;
    if (data_pump_n > 2) return;
    // Deliver 5 bytes as the waiter sleeps (like cb_network would).
    for (int i = 0; i < MAX_PCBS; i++) {
        if (pcb_used[i] && pcb_pool[i].recv_cb && !pcb_pool[i].freed) {
            struct pbuf *p = make_pbuf(5, 0x41);
            pcb_pool[i].recv_cb(pcb_pool[i].cb_arg, &pcb_pool[i], p, ERR_OK);
            return;
        }
    }
}

static void t_wake_on_data(void) {
    reset_world();
    int h = open_connected(P1, CK1);
    CHECK(h > 0);
    data_pump_n = 0;
    int sleeps_before = test_sleep_count;
    test_pump = data_pump;
    char tmp[64];
    int n = ksock_recv(h, tmp, sizeof(tmp), P1, CK1);
    test_pump = NULL;
    CHECK(n == 5);
    CHECK(memcmp(tmp, "ABCDE", 5) == 0);
    CHECK(test_sleep_count - sleeps_before <= 3);  // woken early, no 10s burn
    CHECK(test_unblock_count >= 1);                // waiter woken by callback
    CHECK(ksock_close(h, P1, CK1) == 0);
}

// ---- Test 8: TCP RX flood — no ack-then-drop, order preserved ----

static void t_rx_flood(void) {
    reset_world();
    int h = open_connected(P1, CK1);
    CHECK(h > 0);
    struct tcp_pcb *pcb = last_pcb;
    // 40 x 1460 = 58400 bytes, far beyond the old 4K ring and the 32K cap.
    for (uint32_t i = 0; i < 40; i++)
        pcb->recv_cb(pcb->cb_arg, pcb, make_pbuf(1460, i * 1460), ERR_OK);
    // Bounded, and NOTHING acked before userspace consumed it:
    CHECK(socks[h & 7].rx_bytes == 22u * 1460u);   // 32120 queued (floor)
    CHECK(pcb->recved_total == 0);
    // Drain: every byte in order, every byte acked exactly once.
    uint8_t buf[8192];
    uint32_t got = 0;
    for (;;) {
        // recv would block when drained; stop via internal counter instead.
        if (socks[h & 7].rx_bytes == 0) break;
        test_pump = NULL;
        // Temporarily shrink deadline pressure: single recv each loop.
        int n = ksock_recv(h, buf, sizeof(buf), P1, CK1);
        CHECK(n > 0);
        for (int i = 0; i < n; i++)
            CHECK(buf[i] == (uint8_t)((got + (uint32_t)i) & 0xFF));
        got += (uint32_t)n;
        if (got > 40000) break;   // safety
    }
    CHECK(got == 22u * 1460u);
    CHECK(pcb->recved_total == got);               // acked == consumed, never more
    // FIN after drain -> clean 0.
    pcb->recv_cb(pcb->cb_arg, pcb, NULL, ERR_OK);
    test_pump = NULL;
    uint64_t t0 = test_ms;
    int n = ksock_recv(h, buf, sizeof(buf), P1, CK1);
    CHECK(n == 0 && test_ms == t0);                // immediate, no 10s wait
    CHECK(ksock_close(h, P1, CK1) == 0);
    CHECK(pbuf_live == 0);
}

// ---- Test 9: normal TCP client regression ----

static void t_normal_tcp(void) {
    reset_world();
    int h = ksock_socket(P1, CK1);
    CHECK(h > 0);
    test_pump = pump_connect_ok;
    CHECK(ksock_connect(h, 0x0202000Au, 7, P1, CK1) == 0);
    test_pump = NULL;
    const char *msg = "halo dari kyuzen";
    CHECK(ksock_send(h, msg, 16, P1, CK1) == 16);
    CHECK(last_pcb->sent_total == 16);
    struct pbuf *p = make_pbuf(6, 0);
    memcpy(p->payload, "dunia!", 6);
    last_pcb->recv_cb(last_pcb->cb_arg, last_pcb, p, ERR_OK);
    char rbuf[32];
    CHECK(ksock_recv(h, rbuf, sizeof(rbuf), P1, CK1) == 6);
    CHECK(memcmp(rbuf, "dunia!", 6) == 0);
    CHECK(last_pcb->recved_total == 6);
    CHECK(ksock_close(h, P1, CK1) == 0);
    CHECK(last_pcb->closed == 1);
}

// ---- Misc: timeouts, reown, connect-refused ----

static void t_timeouts(void) {
    reset_world();
    int h = ksock_socket(P1, CK1);
    CHECK(h > 0);
    uint64_t t0 = test_ms;
    CHECK(ksock_connect(h, 1, 80, P1, CK1) == KSOCK_ETIMEOUT);  // never fires
    CHECK(test_ms - t0 >= 5000);
    CHECK(ksock_close(h, P1, CK1) == 0);   // ERRed socket still closable
    // Recv timeout on an idle connection:
    h = open_connected(P1, CK1);
    CHECK(h > 0);
    t0 = test_ms;
    char tmp[16];
    CHECK(ksock_recv(h, tmp, sizeof(tmp), P1, CK1) == KSOCK_ETIMEOUT);
    CHECK(test_ms - t0 >= 10000);
    CHECK(ksock_close(h, P1, CK1) == 0);
}

static void t_reown(void) {
    reset_world();
    int h = ksock_socket(P1, CK1);
    CHECK(h > 0);
    net_task_reown(P1, 777u);                       // exec renewed the cookie
    CHECK(ksock_close(h, P1, CK1) == KSOCK_EOWNER);  // old identity dead
    CHECK(ksock_close(h, P1, 777u) == 0);           // sockets survive exec
}

static void t_conn_refused(void) {
    reset_world();
    int h = ksock_socket(P1, CK1);
    CHECK(h > 0);
    test_connect_err = ERR_CONN;                    // tcp_connect fails now
    CHECK(ksock_connect(h, 1, 80, P1, CK1) == KSOCK_ECONN);
    CHECK(ksock_close(h, P1, CK1) == 0);
}

int main(void) {
    cur_test = "close_reuse";       t_close_reuse();
    cur_test = "exit_cleanup";      t_exit_cleanup();
    cur_test = "repeated_exit";     t_repeated_exit();
    cur_test = "eight_cycles";      t_eight_cycles();
    cur_test = "cross_process";     t_cross_process();
    cur_test = "stale_pcb";         t_stale_pcb();
    cur_test = "kill_while_waiting"; t_kill_while_waiting();
    cur_test = "wake_on_data";      t_wake_on_data();
    cur_test = "rx_flood";          t_rx_flood();
    cur_test = "normal_tcp";        t_normal_tcp();
    cur_test = "timeouts";          t_timeouts();
    cur_test = "reown";             t_reown();
    cur_test = "conn_refused";      t_conn_refused();
    if (failures == 0) printf("netsock_test: all 13 groups PASS\n");
    else printf("netsock_test: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
