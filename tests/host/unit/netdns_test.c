// netdns_test — host regression for sys_resolve backend.
//
// Compiles the REAL kernel/net/net_dns.c against a fake lwIP dns.h
// (tests/host/unit/netstub/lwip/dns.h) plus timer/sleep/lock mocks, then
// drives it deterministically: cached answer, async answer, negative
// answer, never-answer (timeout), resolver misuse, and arg validation.
//
//   make test-netdns

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spinlock.h"   // mock (no cli/sti); same -iquote pattern as netsock
spinlock_t g_kernel_lock;

// ---- mocks ----
static uint64_t test_ms;
static int      test_sleep_count;
static void   (*test_pump)(void);

uint64_t timer_get_ms(void) { return test_ms; }
void task_sleep_ms(uint32_t ms) {
    test_sleep_count++;
    test_ms += ms;
    if (test_pump) test_pump();
}
static uint64_t mock_lock_saved;
void net_lock_acquire(uint64_t *saved) { *saved = 0; (void)mock_lock_saved; }
void net_lock_release(uint64_t saved) { (void)saved; }

// ---- fake lwIP dns ----
#include "lwip/dns.h"
#include "lwip/err.h"

typedef enum { DNS_CACHED, DNS_ASYNC_OK, DNS_ASYNC_NEG, DNS_NEVER, DNS_MISUSE } dns_mode_t;
static dns_mode_t   test_mode;
static uint32_t     test_answer = 0x01020304u;
static int          test_calls;
static dns_found_callback test_saved_cb;
static int          test_fire_after = -1;   // pump countdown, -1 = manual/never

err_t dns_gethostbyname(const char *hostname, ip_addr_t *addr,
                        dns_found_callback found, void *arg) {
    (void)hostname; (void)arg;
    test_calls++;
    if (test_mode == DNS_CACHED) {
        addr->addr = test_answer;
        return ERR_OK;
    }
    if (test_mode == DNS_MISUSE) return -16;   // ERR_ARG
    test_saved_cb = found;
    return ERR_INPROGRESS;
}

static void pump_fire_ok(void) {
    if (test_fire_after < 0 || !test_saved_cb) return;
    if (--test_fire_after == 0) {
        ip_addr_t a; a.addr = test_answer;
        test_saved_cb("h", &a, NULL);
    }
}
static void pump_fire_neg(void) {
    if (test_fire_after < 0 || !test_saved_cb) return;
    if (--test_fire_after == 0) test_saved_cb("h", NULL, NULL);
}

// ---- the real thing ----
#define NETDNS_HOST_TEST 1
#include "../../../kernel/net/net_dns.c"

// ---- framework ----
static int failures;
static const char *cur_test;
#define CHECK(c) do { \
    if (!(c)) { printf("FAIL [%s] line %d: %s\n", cur_test, __LINE__, #c); failures++; } \
} while (0)

static void reset_world(void) {
    test_ms = 1000000; test_sleep_count = 0; test_pump = NULL;
    test_calls = 0; test_saved_cb = NULL; test_fire_after = -1;
    test_mode = DNS_CACHED;
}

#define T(name) static void name(void)

T(t_null_args) {
    cur_test = "null_args";
    uint32_t ip = 0xDEADu;
    CHECK(net_dns_resolve(NULL, &ip) == KSOCK_ERR);
    CHECK(net_dns_resolve("h", NULL) == KSOCK_ERR);
    CHECK(test_calls == 0);
    CHECK(ip == 0xDEADu);
}

T(t_empty_host) {
    cur_test = "empty_host";
    uint32_t ip = 0xDEADu;
    CHECK(net_dns_resolve("", &ip) == KSOCK_ERR);
    CHECK(test_calls == 0);
    CHECK(ip == 0xDEADu);
}

T(t_overlong_host) {
    cur_test = "overlong_host";
    char big[200];
    memset(big, 'a', sizeof(big) - 1); big[sizeof(big) - 1] = '\0';
    uint32_t ip = 0xDEADu;
    CHECK(net_dns_resolve(big, &ip) == KSOCK_ERR);
    CHECK(test_calls == 0);
    CHECK(ip == 0xDEADu);
}

T(t_cached) {
    cur_test = "cached";
    uint32_t ip = 0;
    test_mode = DNS_CACHED;
    CHECK(net_dns_resolve("example.com", &ip) == KSOCK_OK);
    CHECK(ip == test_answer);
    CHECK(test_sleep_count == 0);   // no wait on cache hit
}

T(t_async_ok) {
    cur_test = "async_ok";
    uint32_t ip = 0;
    test_mode = DNS_ASYNC_OK;
    test_fire_after = 3;
    test_pump = pump_fire_ok;
    CHECK(net_dns_resolve("example.com", &ip) == KSOCK_OK);
    CHECK(ip == test_answer);
    CHECK(test_sleep_count >= 3);
}

T(t_async_negative) {
    cur_test = "async_negative";
    uint32_t ip = 0xDEADu;
    test_mode = DNS_ASYNC_NEG;
    test_fire_after = 2;
    test_pump = pump_fire_neg;
    CHECK(net_dns_resolve("nope.invalid", &ip) == KSOCK_ECONN);
    CHECK(ip == 0xDEADu);           // untouched on failure
}

T(t_timeout) {
    cur_test = "timeout";
    uint32_t ip = 0xDEADu;
    test_mode = DNS_NEVER;
    test_pump = NULL;
    uint64_t t0 = test_ms;
    CHECK(net_dns_resolve("slow.invalid", &ip) == KSOCK_ETIMEOUT);
    CHECK(ip == 0xDEADu);
    CHECK(test_ms - t0 >= NET_DNS_TIMEOUT_MS);   // full deadline waited
}

T(t_misuse) {
    cur_test = "misuse";
    uint32_t ip = 0xDEADu;
    test_mode = DNS_MISUSE;
    CHECK(net_dns_resolve("h", &ip) == KSOCK_ERR);
    CHECK(ip == 0xDEADu);
}

T(t_busy_released) {
    cur_test = "busy_released";
    // After any resolution (even timeout), the next one must still work:
    // the internal lock is always released.
    test_mode = DNS_NEVER;
    uint32_t ip = 0;
    CHECK(net_dns_resolve("a", &ip) == KSOCK_ETIMEOUT);
    test_mode = DNS_CACHED;
    CHECK(net_dns_resolve("b", &ip) == KSOCK_OK);
    CHECK(ip == test_answer);
}

int main(void) {
    reset_world(); t_null_args();
    reset_world(); t_empty_host();
    reset_world(); t_overlong_host();
    reset_world(); t_cached();
    reset_world(); t_async_ok();
    reset_world(); t_async_negative();
    reset_world(); t_timeout();
    reset_world(); t_misuse();
    test_ms = 1000000; test_sleep_count = 0; test_pump = NULL;
    test_calls = 0; test_saved_cb = NULL; test_fire_after = -1;
    t_busy_released();
    if (failures == 0) printf("netdns: ALL PASS\n");
    else printf("netdns: %d FAILURES\n", failures);
    return failures != 0;
}
