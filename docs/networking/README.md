# Networking

KyuzenOS provides a TCP/IP stack built on **lwIP** (in `NO_SYS` mode) with an
Intel **e1000** NIC driver, a kernel socket layer, DHCP/DNS, and ICMP ping.

## Overview

| Layer | Location | Responsibility |
| --- | --- | --- |
| NIC driver | `drivers/net/e1000/` | Intel e1000 (82540EM) TX/RX descriptor rings |
| lwIP glue | `drivers/net/port/` | `kyuzen_netif` — the netif interface to lwIP |
| Kernel network | `kernel/net/` | Init, sockets, ping, DNS |
| Userspace wrappers | `libs/core/netutil.c` | Address parsing, resolve, dial helpers |

| Document | Description |
| --- | --- |
| [Socket Model](sockets.md) | Ownership, handles, blocking, lifecycle |
| [Network Stack](stack.md) | lwIP integration, e1000 driver, DHCP/DNS/ping |

## Supported Capabilities

| Capability | Status |
| --- | --- |
| IPv4 | Currently supported |
| DHCP | Currently supported (with static fallback) |
| DNS resolution | Currently supported (syscall 86) |
| ICMP ping | Currently supported (syscall 41) |
| TCP client sockets | Currently supported |
| TCP server / listen | Not currently implemented |
| UDP | Not currently implemented at the socket API level |
| IPv6 | Not currently implemented |
| TLS | Implemented in userspace (BearSSL), not in the kernel — see [Browser](../browser/README.md) |

## Configuration

- **Interface**: e1000 NIC (`-nic user,model=e1000` in QEMU).
- **Addressing**: DHCP by default; if DHCP fails, a static fallback address is
  used.
- **Threading model**: `NO_SYS=1` — lwIP runs without an OS abstraction layer.
  A single global `net_lock` protects all lwIP state, and network polling is
  driven from the timer callback (`cb_network`) on the boot CPU.

## Network Syscalls

| # | Name | Args | Returns |
| --- | --- | --- | --- |
| 41 | `ping` | host string | RTT or error |
| 52 | `socket` | type | handle or error |
| 53 | `connect` | handle, addr, port | 0 / error |
| 54 | `sock_send` | handle, buf, len | bytes sent / error |
| 55 | `sock_recv` | handle, buf, len | bytes received / error |
| 56 | `sock_close` | handle | 0 / error |
| 86 | `resolve` | host, out | 0 / error |

See [Syscall Table](../reference/syscalls.md) and
[Socket Model](sockets.md) for details.

## Socket Error Codes

| Code | Value | Meaning |
| --- | --- | --- |
| `KSOCK_OK` | 0 | Success |
| `KSOCK_ERR` | -1 | Generic error |
| `KSOCK_EHANDLE` | -2 | Invalid handle |
| `KSOCK_EOWNER` | -3 | Wrong owning process |
| `KSOCK_ETIMEOUT` | -4 | Timeout |
| `KSOCK_ECONN` | -5 | Connection error |
| `KSOCK_ECLOSED` | -6 | Closed |
| `KSOCK_ENOMEM` | -7 | Out of memory |

## Current Limitations

- **TCP client only.** There is no `listen`/`accept`; the socket layer supports
  outgoing connections.
- **No UDP socket API.** ICMP ping and DNS use lwIP raw APIs internally.
- **Single global network lock.** lwIP is not SMP-safe; all access is
  serialized.
- **DNS timeout** is 5 seconds (`NET_DNS_TIMEOUT_MS`), polled in 20 ms
  quanta (`NET_DNS_WAIT_QUANTUM_MS`).
- **No socket options** (`setsockopt`), no non-blocking mode flag beyond the
  documented blocking behavior.

## Development Notes

- Any code that touches lwIP state must hold `net_lock`. This includes the ping
  path, the socket path, and the timer-driven poll. Violating this was the
  subject of a historical audit; all known paths now comply.
- The e1000 TX/RX rings use PMM-allocated pages accessed through the HHDM, not
  `kmalloc` — the kernel heap is a virtual mapping that cannot be converted to a
  physical DMA address.
- The driver has no internal mutex; correctness depends entirely on `net_lock`.

## Related Documentation

- [Socket Model](sockets.md)
- [Network Stack](stack.md)
- [Syscall Table](../reference/syscalls.md)
- [Drivers](#) — e1000 and PCI
