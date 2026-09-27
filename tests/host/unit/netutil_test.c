// netutil_test — host regression for the reusable client helpers.
//
// Compiles the REAL libs/core/netutil.c with stubbed sys_* calls.
// Covers IPv4 literal parsing (accept + reject shapes), resolve fast-path
// vs syscall routing, and dial success/failure cleanup (no leaked socket).
//
//   make test-netutil

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- stub syscalls (observed by the test) ----
static int stub_resolve_rc = 0;
static uint32_t stub_resolve_ip = 0x05060708u;
static int stub_resolve_calls;
static char stub_resolve_last[160];

static int stub_socket_rc = 7;
static int stub_socket_calls;
static int stub_connect_rc = 0;
static int stub_connect_calls;
static int stub_close_calls;
static int stub_close_last = -99;

int sys_resolve(const char *host, uint32_t *out_ip_be) {
    stub_resolve_calls++;
    strncpy(stub_resolve_last, host, sizeof(stub_resolve_last) - 1);
    stub_resolve_last[sizeof(stub_resolve_last) - 1] = '\0';
    if (stub_resolve_rc == 0) *out_ip_be = stub_resolve_ip;
    return stub_resolve_rc;
}
int sys_socket(void) { stub_socket_calls++; return stub_socket_rc; }
int sys_connect(int s, uint32_t ip_be, uint16_t port) {
    (void)s; (void)ip_be; (void)port;
    stub_connect_calls++;
    return stub_connect_rc;
}
int sys_sock_close(int s) { stub_close_calls++; stub_close_last = s; return 0; }

// userlib.h is NOT included: netutil.c includes it, but the test provides
// the sys_* symbols above. Provide the header guard manually is wrong —
// instead just include the real headers the same way (they are host-safe).
// NOTE: netutil.c does `#include "userlib.h"`; that header is host-safe
// (stdint + ABI structs only), so -iquote include suffices.

// ---- the real thing ----
#include "../../../libs/core/netutil.c"

// ---- framework ----
static int failures;
static const char *cur_test;
#define CHECK(c) do { \
    if (!(c)) { printf("FAIL [%s] line %d: %s\n", cur_test, __LINE__, #c); failures++; } \
} while (0)

static void reset_world(void) {
    stub_resolve_rc = 0; stub_resolve_calls = 0; stub_resolve_last[0] = '\0';
    stub_socket_rc = 7; stub_socket_calls = 0;
    stub_connect_rc = 0; stub_connect_calls = 0;
    stub_close_calls = 0; stub_close_last = -99;
}

#define T(name) static void name(void)

T(t_parse_valid) {
    cur_test = "parse_valid";
    uint32_t ip = 0;
    // Konvensi ip_be = layout memori LE = byte dotted berurutan.
    CHECK(net_parse_ipv4("10.0.2.2", &ip) == 0 && ip == 0x0202000Au);
    CHECK(net_parse_ipv4("0.0.0.0", &ip) == 0 && ip == 0x00000000u);
    CHECK(net_parse_ipv4("255.255.255.255", &ip) == 0 && ip == 0xFFFFFFFFu);
    CHECK(net_parse_ipv4("1.2.3.4", &ip) == 0 && ip == 0x04030201u);
    CHECK(net_parse_ipv4("001.002.003.004", &ip) == 0 && ip == 0x04030201u);
}

T(t_parse_reject) {
    cur_test = "parse_reject";
    uint32_t ip = 0xDEADu;
    const char *bad[] = {
        "", "1.2.3", "1.2.3.4.5", "1.2.3.256", "1.2.3.4.", ".1.2.3.4",
        "1..3.4", "1.2.3.4x", "a.b.c.d", " 1.2.3.4", "1.2.3.4 ",
        "1.2.3.4444", "1.2.3.-1", "0x1.2.3.4", NULL
    };
    for (int i = 0; bad[i]; i++) {
        ip = 0xDEADu;
        if (net_parse_ipv4(bad[i], &ip) != -1) {
            printf("FAIL [parse_reject] accepted: [%s]\n", bad[i]);
            failures++;
        }
        if (ip != 0xDEADu) {
            printf("FAIL [parse_reject] touched out: [%s]\n", bad[i]);
            failures++;
        }
    }
    CHECK(net_parse_ipv4(NULL, &ip) == -1);
    CHECK(net_parse_ipv4("1.2.3.4", NULL) == -1);
}

T(t_resolve_literal) {
    cur_test = "resolve_literal";
    uint32_t ip = 0;
    CHECK(net_resolve("10.0.2.2", &ip) == 0);
    CHECK(ip == 0x0202000Au);
    CHECK(stub_resolve_calls == 0);   // no syscall for literals
}

T(t_resolve_name) {
    cur_test = "resolve_name";
    uint32_t ip = 0;
    CHECK(net_resolve("example.com", &ip) == 0);
    CHECK(ip == stub_resolve_ip);
    CHECK(stub_resolve_calls == 1);
    CHECK(strcmp(stub_resolve_last, "example.com") == 0);
}

T(t_resolve_localhost) {
    cur_test = "resolve_localhost";
    uint32_t ip = 0;
    // loopback lokal: tanpa syscall (jalan saat link down).
    CHECK(net_resolve("localhost", &ip) == 0);
    CHECK(ip == 0x0100007Fu);
    CHECK(stub_resolve_calls == 0);
}

T(t_resolve_fail) {
    cur_test = "resolve_fail";
    uint32_t ip = 0xDEADu;
    stub_resolve_rc = -4;
    CHECK(net_resolve("x.invalid", &ip) == -4);
    CHECK(ip == 0xDEADu);
    CHECK(net_resolve("", &ip) == -1);
    CHECK(net_resolve(NULL, &ip) == -1);
    char big[200];
    memset(big, 'z', sizeof(big) - 1); big[sizeof(big) - 1] = '\0';
    CHECK(net_resolve(big, &ip) == -1);
    CHECK(stub_resolve_calls == 1);   // only the real-name attempt
}

T(t_dial_ok) {
    cur_test = "dial_ok";
    uint32_t ip = 0;
    int s = net_dial("10.0.2.2", 80, &ip);
    CHECK(s == 7);
    CHECK(ip == 0x0202000Au);
    CHECK(stub_socket_calls == 1 && stub_connect_calls == 1);
    CHECK(stub_close_calls == 0);
}

T(t_dial_name_ok_null_ip) {
    cur_test = "dial_name_ok_null_ip";
    int s = net_dial("example.com", 8080, NULL);
    CHECK(s == 7);
    CHECK(stub_resolve_calls == 1);
    CHECK(stub_close_calls == 0);
}

T(t_dial_cleanup) {
    cur_test = "dial_cleanup";
    // resolve fail -> no socket touched
    stub_resolve_rc = -5;
    CHECK(net_dial("x.invalid", 80, NULL) < 0);
    CHECK(stub_socket_calls == 0);
    // socket fail -> propagates, no connect/close
    stub_resolve_rc = 0; stub_socket_rc = -7;
    CHECK(net_dial("h", 80, NULL) == -7);
    CHECK(stub_connect_calls == 0 && stub_close_calls == 0);
    // connect fail -> socket closed, never leaked
    stub_socket_rc = 9; stub_connect_rc = -5;
    CHECK(net_dial("h", 80, NULL) < 0);
    CHECK(stub_close_calls == 1 && stub_close_last == 9);
}

int main(void) {
    reset_world(); t_parse_valid();
    reset_world(); t_parse_reject();
    reset_world(); t_resolve_literal();
    reset_world(); t_resolve_localhost();
    reset_world(); t_resolve_name();
    reset_world(); t_resolve_fail();
    reset_world(); t_dial_ok();
    reset_world(); t_dial_name_ok_null_ip();
    reset_world(); t_dial_cleanup();
    if (failures == 0) printf("netutil: ALL PASS\n");
    else printf("netutil: %d FAILURES\n", failures);
    return failures != 0;
}
