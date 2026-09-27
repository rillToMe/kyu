#ifndef NET_DNS_H
#define NET_DNS_H

#include <stdint.h>
#include "net_socket.h"   // KSOCK_* error codes (resolve reuses them)

// Syscall 86: sys_resolve(host*, out_ip_be*) -> 0 / negative KSOCK_*.
// Resolves a hostname to IPv4 (network byte order) via the lwIP resolver.
// Errors: KSOCK_ERR (bad args / resolver misuse), KSOCK_ETIMEOUT (5s
// deadline), KSOCK_ECONN (negative answer / no data). IP literals are NOT
// handled here — use net_parse_ipv4() (netutil) first, it costs no syscall.
#define SYS_RESOLVE 86

#define NET_DNS_TIMEOUT_MS 5000u       // wait deadline (matches ping/connect)
#define NET_DNS_WAIT_QUANTUM_MS 20u    // sleep slice (matches KSOCK quantum)
#define NET_DNS_MAX_HOST 128u          // == UC_MAX_HOST (syscall copy cap)

// host = kernel pointer, NUL-terminated, non-empty, <= NET_DNS_MAX_HOST.
// out_ip_be = kernel u32 sink. Blocking, kill-interruptible; concurrent
// callers serialize on an internal lock (sleep-based, never spins under
// kill risk). Returns KSOCK_OK / KSOCK_ERR / KSOCK_ETIMEOUT / KSOCK_ECONN.
int net_dns_resolve(const char *host, uint32_t *out_ip_be);

#endif // NET_DNS_H
