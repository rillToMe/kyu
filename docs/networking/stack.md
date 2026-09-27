# Network Stack

This document describes the internal architecture of the KyuzenOS network
stack: the lwIP integration, the e1000 driver, and the DHCP/DNS/ping paths.

## Component Diagram

```text
userspace app (netutil.c)
    │  syscalls 41 / 52–56 / 86
    ▼
kernel/net/net_socket.c      (ksock_* layer, handle table, net_lock)
kernel/net/net_ping.c        (raw ICMP echo)
kernel/net/net_dns.c         (hostname resolution)
    │
    ▼
lwIP (third_party/net/lwip)  — NO_SYS=1, raw API
    │
    ▼
kyuzen_netif (drivers/net/port/kyuzen_netif.c)   — lwIP netif
    │
    ▼
e1000 driver (drivers/net/e1000/e1000.c)          — TX/RX descriptor rings
    │
    ▼
Intel e1000 NIC hardware
```

## lwIP Integration

lwIP runs in `NO_SYS=1` mode: there is no OS abstraction layer, no threads
inside lwIP, and no blocking. All lwIP state is protected by a single global
spinlock, `net_lock` (`kernel/net/net_socket.c`).

### Polling model

lwIP is driven cooperatively:

- The timer callback `cb_network` (`kernel/timer_callbacks.c`) runs on each
  tick. It acquires `net_lock`, calls `e1000_poll()` to reap received frames,
  and calls `sys_check_timeouts()` to advance lwIP timers.
- This runs in interrupt context (the timer IRQ). `net_lock` is held only
  briefly, and no lwIP callback re-acquires it, so there is no reentrancy
  deadlock. This is a documented design decision; a redesign was evaluated and
  judged unnecessary.

### Why the lock matters

lwIP is explicitly not SMP-safe. Any code path that mutates lwIP structures
(raw PCBs, pbuf pools, the netif) must hold `net_lock`. Historically the ping
path did not, which could corrupt lwIP state under SMP; all known paths now
comply.

## The e1000 Driver

`drivers/net/e1000/e1000.c` drives the Intel 82540EM (and compatible) NIC.

### Descriptor rings

- **TX/RX rings**: descriptor arrays plus packet buffers, allocated from PMM
  physical pages.
- **DMA address computation**: because the kernel heap is a virtual mapping
  (`0xFFFF9000…`) that cannot be converted to a physical address by subtracting
  the HHDM offset, the driver allocates DMA buffers from the PMM and accesses
  them via `phys + hhdm_offset`. Using `kmalloc` for DMA was a historical bug:
  the NIC received bogus addresses, TX descriptors never completed, and the log
  filled with `[e1000] WARN: TX ring full`.

### Send/receive

- `e1000_send` writes the next TX descriptor and advances the tail register
  (`TDT`).
- `e1000_poll` reaps completed RX descriptors and delivers frames to lwIP.

### Threading

The driver has **no internal mutex**. Correctness depends entirely on callers
holding `net_lock`. All current callers do.

## Socket Layer (ksock)

`kernel/net/net_socket.c` implements the kernel-side socket table used by
syscalls 52–56. See [Socket Model](sockets.md) for the ownership and handle
design.

## DHCP

`kernel/net/net_init.c` initializes lwIP, brings up the netif, and starts DHCP.
If DHCP fails within its timeout, a static fallback address is configured.

> The DHCP wait loop uses a consistent `sti; hlt` pattern. An earlier bare
> `hlt` (without `sti`) could hang if interrupts were disabled; this was fixed.

## DNS

`kernel/net/net_dns.c` resolves hostnames via lwIP's `dns_gethostbyname` and
exposes the result through syscall 86 (`resolve`).

| Constant | Value |
| --- | --- |
| `NET_DNS_TIMEOUT_MS` | 5000 |
| `NET_DNS_WAIT_QUANTUM_MS` | 20 |
| `NET_DNS_MAX_HOST` | 128 |

The resolver supports cached results, asynchronous requests, and negative
caching. It validates its arguments and releases `net_lock` while waiting.

## ICMP Ping

`kernel/net/net_ping.c` implements ICMP echo request/reply using lwIP's raw
API. The shell `ping <host>` command drives it (syscall 41).

Design notes:

- All lwIP calls in the ping path are wrapped in
  `net_lock_acquire`/`net_lock_release`, and the lock is released before each
  `sti; hlt` so the boot-CPU poll can run.
- Ping state is serialized by a spinlock (`ping_busy`), so two concurrent pings
  do not corrupt the shared global state.
- Packet-length validation uses `p->tot_len` (the whole pbuf) plus a
  first-segment check on `p->len`, so the ICMP header dereference is safe on
  chained pbufs.

## Entropy

Randomness for TLS and other uses comes from `kernel/entropy.c`, exposed via
syscall 87.

| Constant | Value |
| --- | --- |
| `ENTROPY_MAX` | 256 |
| `ENTROPY_ERR` | -1 |
| `ENTROPY_ENOHW` | -2 |

The source is `RDRAND`. There is no fallback source; a CPU without `RDRAND`
returns `ENTROPY_ENOHW`.

## Related Documentation

- [Networking Overview](README.md)
- [Socket Model](sockets.md)
- [Syscall Table](../reference/syscalls.md)
- [Interrupts & Timers](../kernel/interrupts.md) — the timer-driven poll
