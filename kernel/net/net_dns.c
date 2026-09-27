// kernel/net/net_dns.c — userspace hostname resolver over lwIP DNS.
//
// The resolver already runs (servers from DHCP/static, net_init.c); only
// kernel_ping consumed it. This exposes it to ring-3 via sys_resolve(86)
// WITHOUT duplicating DNS logic: same dns_gethostbyname + callback + wait
// discipline as net_ping.c, but with ksock-style blocking (task_sleep_ms
// quanta, kill-interruptible) instead of sti_hlt, and an internal busy
// lock so concurrent resolvers never share callback state.
//
// LOCKING (mirrors net_socket.c / net_ping.c):
// - lwIP entry under net_lock (public acquire/release; net_lock is static
//   in net_socket.c). Callbacks fire from the locked BSP poll and never
//   re-acquire.
// - net_lock is never held across a sleep. The wait loop runs outside it.
// - resolve_busy serializes whole resolutions (globals dns_done/result).
//   Acquisition is sleep-based (try + task_sleep_ms quantum): a spinning
//   acquire inside a syscall would ignore kill_pending forever.
// - Kill path: task_sleep_ms() observes kill_pending and converges on
//   proc_exit_kill() (noreturn) — a killed resolver never returns here,
//   and resolve_busy is a plain spinlock with no owner to clean up.

#include "net_dns.h"
#include "spinlock.h"
#include <stddef.h>

#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"

extern uint64_t timer_get_ms(void);
extern void task_sleep_ms(uint32_t ms);   // BLOCKED/SLEEPING + kill observation
extern void net_lock_acquire(uint64_t *saved);
extern void net_lock_release(uint64_t saved);

// Syscalls enter with IF=0 (int 0x80 clears it); re-enable so the timer
// poll that drives lwIP/DNS can fire while we block. No-op under host test.
#ifdef NETDNS_HOST_TEST
#define NETDNS_STI() ((void)0)
#else
#define NETDNS_STI() __asm__ volatile("sti" ::: "memory")
#endif

static spinlock_t resolve_busy = SPINLOCK_INIT;

static volatile int      dns_done   = 0;
static volatile uint32_t dns_result = 0;

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name; (void)arg;
    dns_result = (ipaddr != NULL) ? ipaddr->addr : 0;
    dns_done   = 1;
}

int net_dns_resolve(const char *host, uint32_t *out_ip_be) {
    if (!host || !out_ip_be) return KSOCK_ERR;
    if (host[0] == '\0') return KSOCK_ERR;
    // Bounded length: the syscall layer caps at UC_MAX_HOST, but a kernel
    // caller could pass anything — never hand lwIP an unbounded string.
    {
        uint32_t n = 0;
        while (n <= NET_DNS_MAX_HOST && host[n] != '\0') n++;
        if (n == 0 || n > NET_DNS_MAX_HOST) return KSOCK_ERR;
    }

    NETDNS_STI();

    // Serialize (sleep-based acquire: kill-convergent, never a hard spin).
    // try_lock_irqsave returns with the lock HELD + flags on success.
    uint64_t bf = 0;
    for (;;) {
        if (spinlock_try_lock_irqsave(&resolve_busy, &bf)) break;
        task_sleep_ms(NET_DNS_WAIT_QUANTUM_MS);
    }

    int rc = KSOCK_ERR;
    ip_addr_t ip;
    dns_done   = 0;
    dns_result = 0;

    // dns_gethostbyname mutates lwIP state -> under net_lock. The callback
    // fires from the locked BSP poll, so the lock MUST be released before
    // waiting for dns_done.
    err_t derr;
    {
        uint64_t lf;
        net_lock_acquire(&lf);
        derr = dns_gethostbyname(host, &ip, dns_found_cb, NULL);
        if (derr == ERR_OK) {
            dns_result = ip.addr;
            dns_done   = 1;
        }
        net_lock_release(lf);
    }

    if (derr == ERR_OK) {
        rc = KSOCK_OK;
    } else if (derr == ERR_INPROGRESS) {
        uint64_t deadline = timer_get_ms() + NET_DNS_TIMEOUT_MS;
        while (!dns_done && timer_get_ms() < deadline)
            task_sleep_ms(NET_DNS_WAIT_QUANTUM_MS);
        if (!dns_done)
            rc = KSOCK_ETIMEOUT;
        else if (dns_result == 0)
            rc = KSOCK_ECONN;      // negative answer / no data
        else
            rc = KSOCK_OK;
    } else {
        rc = KSOCK_ERR;            // resolver misuse (bad args/table full)
    }

    if (rc == KSOCK_OK) *out_ip_be = dns_result;

    spinlock_unlock_irqrestore(&resolve_busy, bf);
    return rc;
}
