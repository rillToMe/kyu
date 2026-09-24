# KyuzenOS Network Stack Audit

Audit-first, no implementation. Status tokens: `[OK] [PARTIAL] [STUB] [MISSING] [BROKEN] [UNTESTED] [DEFERRED] [UNKNOWN]`.
Every claim cites `file:line` or a verified grep/build result. `third_party/net/lwip` internals cited only to separate "lwIP has it" from "KyuzenOS exposes it".

Build check: `make` (incremental, msys GNU Make 4.4.1) → `EXIT=0`, `build/bin/myos.bin` + net objects present.
`git status` shows only pre-existing unrelated modifications (apps/terminal.c, proc, shell, desktop test) — untouched by this audit. New files here are the only additions, uncommitted.

## 1. Executive Summary

Single-NIC IPv4 TCP-client + ping box. One e1000 driver (polling, no IRQ), one lwIP `netif` (`NO_SYS=1`, raw API only), one global 8-slot kernel socket table, 6 syscalls (41, 52–56), two shell commands (`ping`, `nettest`). No UDP/TCP-server/DNS/IPv6/loopback/TLS/HTTP/Wi-Fi/virtio-net user API. Sockets bypass the VFS FD layer entirely and leak on task exit. SMP safety = one coarse `net_lock` (correct, contended). No automated net tests.

## 2. Current Architecture (actual, not aspirational)

```text
shell (kernel task) / ring-3 ELF
  │  libs/core/userlib.c (int 0x80 wrappers)   │  libs/core/kernel_userlib.c (direct calls, kernel tasks)
  ▼
int 0x80, RAX=num RBX/RCX/RDX=args            kernel/syscall/syscall.c:111 → sys_net_handle
  ▼
kernel/syscall/sys_net.c (bounce buffers via usercopy.h, UC_MAX_SOCK=64K)
  ├─ 41 → kernel_ping()                        kernel/net/net_ping.c (raw ICMP + dns_gethostbyname)
  └─ 52-56 → ksock_*                           kernel/net/net_socket.c (tcp_* raw callbacks, 8 slots)
  ▼
lwIP NO_SYS=1 raw API (tcp.c udp.c dhcp.c dns.c etharp.c ethernet_input …)
  ▼
drivers/net/port/kyuzen_netif.c (linkoutput/low_level_output, e1000_rx_callback, netif "kz")
  ▼
drivers/net/e1000/e1000.c (TX/RX rings 16 descs, PMM DMA pages, BAR0+HHDM, polling only)
  ▼
PCI CF8/CFC self-rolled 32-bit access → QEMU `-nic user,model=e1000` (slirp 10.0.2.0/24)
```

Pump: `cb_network` in `kernel/timer_callbacks.c:150` runs every timer tick under `net_lock`: `e1000_poll()` + `sys_check_timeouts()` (≤1/ms). All lwIP entry (any CPU) serialized by `net_lock` (`include/net_socket.h`, impl `kernel/net/net_socket.c:36-39`). Lock never held across `sti;hlt` — blocking syscalls poll with lock released.

## 3. Build Integration

Makefile (`Makefile:44-…`, LWIP section ~lines 60-130, rules ~178-240):

- Compiled: `third_party/net/lwip/src/core/*.c` + `core/ipv4/*.c` + `netif/ethernet.c` + `port/kyuzen_netif.c` + `port/sys_arch.c` + `drivers/net/e1000/e1000.c` + `kernel/net/{net_init,net_ping,net_socket}.c`.
- `LWIP_CFLAGS` (kernel CFLAGS + `-I lwip/src/include` + `-I drivers/net/port` + `-Wno-error`) applies ONLY to lwIP core + port + the 3 kernel/net files. e1000 uses plain kernel CFLAGS (no lwIP headers).
- NOT compiled (exist but dead): `core/ipv6/*`, `netif/*` except ethernet.c (loopif/slip/ppp), `api/*` (sockets.c, netconn, netbuf), `apps/*`. Source-exists ≠ linked.
- `sys_arch.c` in `NO_SYS=1` = only `sys_now()` → `timer_get_ms()` (PIT). Correct per lwIP NO_SYS contract.

## 4. Network Drivers (e1000 only)

`drivers/net/e1000/e1000.{h,c}`. No virtio-net (only virtio-gpu exists), no rtl8139, no Wi-Fi.

- Detection (`e1000.c:380-404`): PCI bus 0–7, slot 0–31, func **0 only**, vendor `0x8086`, devices `0x100E/0x100F/0x10D3` (`e1000.h:28-31`). Class code not checked.
- BAR (`e1000.c:424-441`): BAR0, 32/64-bit handled, phys+HDM → MMIO. Own CF8/CFC 32-bit accessors (`e1000.c:83-106`); `drivers/pci.c` 16-bit API bypassed. Bus-master + MEM-space enabled (`e1000.c:109-113`).
- Reset: SW RST + poll clear (`e1000.c:445-452`), link forced up SLU|ASDE (`e1000.c:456-460`).
- DMA (`e1000.c:158-167`): one PMM 4K page per buffer + one page per ring; phys addr in descriptors, virt = phys+HHDM. Rings: 16 TX + 16 RX descs (`e1000.h:179-180`). Assumes x86 cache-coherent DMA (valid), no flush; no IOMMU handling.
- TX (`e1000.c:484-522`): DD-bit check → memcpy to DMA buf → EOP|IFCS|RS → sfence → TDT kick. Full ring = `-1` + warn, no retry/queue.
- RX (`e1000.c:547-581`): `e1000_poll()` walks tail+1…RDH, requires DD+EOP, errors==0, len>0; hands to weak `e1000_rx_callback` overridden in `kyuzen_netif.c:187`. Descriptors recycled via RDT. RX-overrun (ICR_RXO) never read.
- IRQ: IMS programmed RXT0|LSC|RXDMT0 (`e1000.c:474`) but **no IDT handler wired** — pure polling from `cb_network`. `[STUB]` interrupt config, polling reality.
- Link: `e1000_link_up()` reads STATUS bit1 (`e1000.c:595-599`) but nothing calls it; `NETIF_FLAG_LINK_UP` hardcoded (`kyuzen_netif.c:224-227`). Link-down undetectable at runtime.
- MAC: EEPROM EERD words 0-2, RAL/RAH fallback (`e1000.c:218-240`). `[OK]`.
- MTU 1500 hardcoded (`kyuzen_netif.c:69`); `low_level_output` drops >1518 (`kyuzen_netif.c:94-98`). RX bufs 2048 (`RCTL_BSIZE_2048`), promiscuous UPE+MPE on (`e1000.c:365-366`).
- Error recovery: none — no NIC reset path, no ring-refill failure handling past init, no device-disappearance/link-loss handling. `[MISSING]`.

## 5. PCI / DMA

Raw CF8/CFC in-driver; no MSI/MSI-X; DMA = PMM pages + HHDM identity assumption documented in `e1000.c:26-30`. Coupling: driver assumes single contiguous low page per buffer (true for 2K bufs in 4K pages), x86 DMA coherence, BAR0 MMIO, func-0 devices. No abstraction for a second NIC (static singleton `e1000`, single `g_kyuzen_netif`).

## 6. lwIP Integration (`drivers/net/port/lwipopts.h`, full file read)

| Component | Present(src) | Compiled | Enabled | Integrated | Tested | Status |
| --------- | --- | --- | --- | --- | --- | --- |
| Ethernet | y | y (ethernet.c) | 1 | y (`ethernet_input`, linkoutput) | manual ping | [PARTIAL] poll-only |
| ARP | y | y (etharp via ipv4/*) | 1 | y (`etharp_output` as netif->output) | indirect via ping/DHCP | [OK] |
| IPv4 | y | y | 1 | y, single netif + default route | manual | [OK] |
| IPv6 | y (src) | **no** | 0 | no | no | [MISSING] |
| ICMP | y | y | 1 | y, raw PCB in net_ping.c | shell ping | [OK] |
| TCP | y | y | 1 | y, client-only raw API | shell nettest | [PARTIAL] no server |
| UDP | y | y | 1 | **no** (zero refs in kernel/, verified grep) | no | [STUB] compiled, unreachable |
| DHCP | y | y | 1 | y, 8s wait + static fallback | boot log | [OK] |
| DNS | y | y | 1 | half: hardcoded 8.8.8.8/1.1.1.1, DHCP offer overwrites (`dhcp.c:743`); only consumer is kernel_ping | ping hostname | [PARTIAL] no user API |
| AutoIP/IGMP/mDNS/SNTP | src varies | no (ipv4/* wildcard WOULD compile autoip.c/igmp.c if present — check: compiled as part of ipv4/*.c but never invoked; no API refs) | — | no | no | [MISSING] effectively |
| Loopback | src | no | 0 (`LWIP_HAVE_LOOPIF 0`) | no | no | [MISSING] |

Pools: MEM 64K, PBUF 16×1536, TCP PCB 8 / listen 4 / SEG 32, MSS 1460, WND=SND=4×MSS, DNS table 4, SW checksums, LCG `LWIP_RAND()` (predictable XIDs — see §24). Socket/netconn APIs compiled out (`LWIP_SOCKET 0`, `LWIP_NETCONN 0`, `api/` unbuilt).

## 7. Packet / Buffer Ownership

- RX: DMA buf → `pbuf_alloc(PBUF_RAW,PBUF_POOL)` + `pbuf_take` (1 copy, `kyuzen_netif.c:130-173`); pool-empty → silent drop + stats. `ethernet_input` frees.
- TX: pbuf chain → 1518B kernel-stack frame (`kyuzen_netif.c:90-101`, 2nd copy incl. DMA memcpy in `e1000_send`). No zero-copy anywhere.
- ksock RX: `cb_recv` copies pbuf payload into 4K per-socket ring (`net_socket.c:48-75`), `tcp_recved` AFTER copy; overflow bytes silently truncated (ring-full → drop, sender still acked — receiver-side data loss under flood). `[PARTIAL]`.
- ksock TX: `TCP_WRITE_FLAG_COPY`, ≤64K-1 chunks (`net_socket.c:165-166`), ERR_MEM → hlt-retry loop w/ 5s deadline.
- Syscall bounce: `sys_net.c:42-68` — copy-in to `kmalloc(len)` (≤64K `UC_MAX_SOCK`), copy-out after recv returns (outside net_lock). TOCTOU-closed per `docs/design/ring3-tahap2-boundary-copy.md`.
- No buffer crosses to userspace by reference; exhaustion handled by drop/timeout, never OOM-panic. No double-free/use-after-free found on these paths (close detaches callbacks before `tcp_close`/abort, `net_socket.c:211-217`).

## 8. Threading / SMP

All lwIP entry under `net_lock` (irqsave spinlock): socket syscalls, ping sections, `cb_network` poll. Callbacks assume lock held, never reacquire. `ping_busy` serializes whole `kernel_ping` (globals `ping_pcb/done/seq`, `net_ping.c:99`). ksock slot-reuse generation counters let stale blockers abort instead of corrupting (`net_socket.c:27,122,219`). Verdict: SMP-correct by coarse locking; expect contention (ping blocks ALL net under `ping_busy`; one `cb_network` does all RX). `[OK]` correctness, `[PARTIAL]` scalability. `sys_now` = volatile PIT ms read — safe.

## 9. Kernel Network Layer

`ksock_t socks[8]`, global, states FREE/ALLOC/CONNECTING/CONNECTED/CLOSED/ERR (`net_socket.c:23-33`). No per-task owner field. `ksock_init()` from `kernel/kernel.c:517-518` (after `net_init()` at `:495`, both post-`sti` at `:461`).

## 10. Syscall ABI

`int 0x80`; RAX=num, RBX/RCX/RDX=args 1-3 (`kernel/syscall/syscall.c:66-72`). Net routed at `syscall.c:111-114` → `sys_net_handle`.

| Syscall | Num | Args | Implemented | User API (ring-3 `libs/core/userlib.c`, kernel `libs/core/kernel_userlib.c`) | Tested |
| --- | --- | --- | --- | --- | --- |
| ping | 41 | RBX=host* | y, 4×echo + DNS, avg RTT / -1 | `sys_ping` (:319); kernel direct `:123` | manual shell |
| socket | 52 | — | y, TCP pcb alloc | `sys_socket` (:477) | manual nettest |
| connect | 53 | RBX=s RCX=ip_be RDX=port | y, 5s block | `sys_connect` (:485) | manual |
| send | 54 | RBX=s RCX=buf RDX=len | y, partial-ok | `sys_send` (:492) | manual |
| recv | 55 | RBX=s RCX=buf RDX=len | y, 10s block, 0=FIN | `sys_recv` (:499) | manual |
| close | 56 | RBX=s | y | `sys_sock_close` (:506) | manual |
| bind/listen/accept/sendto/recvfrom/shutdown/sockopt | — | — | no (no refs anywhere outside lwIP) | none | — |

All errors collapse to `-1`; no errno (`ENOMEM/EAGAIN/ETIMEDOUT/…` never surfaced). `system/shell.c:35-58` = ring-0 wrappers for the in-kernel shell.

## 11. File Descriptor Integration

**None.** ksock 0–7 is a separate global namespace; VFS fds (`kernel/fs/vfs_fd.c`, 16/task) unaware of sockets. `sys_close` ≠ `sys_sock_close`. No dup/inherit; fork/exec interaction undefined (sockets invisible to AS teardown). Any task can use/close any slot — no ownership check in `get()` (`net_socket.c:111-114`).

## 12. TCP — `[PARTIAL]` client-only

Connect/send/recv/close via raw callbacks; retransmit/congestion = lwIP defaults driven by `sys_check_timeouts`. No listen/accept/bind → no server. No keepalive/tuning API. RST only as `tcp_abort` fallback inside `ksock_close`. User app CAN open TCP client connections (proven path: shell `nettest`).

## 13. UDP — `[STUB]`

lwIP UDP compiled (`LWIP_UDP 1`, `MEMP_NUM_UDP_PCB 4`) but unreachable: zero `udp_*` references in kernel/libs/drivers outside vendored lwIP (verified grep). `[MISSING]` user UDP.

## 14. ARP / ICMP / ping — `[OK]` (with scope note)

ARP via `etharp_output`; ICMP echo via kernel raw PCB (`kernel/net/net_ping.c`), syscall 41, shell `ping` (`system/shell_core.c:436`). Fixes verified in-source: sti-before-hlt (IF cleared by INT 0x80), net_lock discipline, `ping_busy`, pbuf chain-length validation (`net_ping.c:128-142`). Ping is a **kernel utility**, not a user-space app. Ping success ≠ TCP/UDP capability.

## 15. DHCP — `[OK]` bringup, `[PARTIAL]` lifecycle

`dhcp_start` + 8s poll loop + `dhcp_stop` + static fallback 10.0.2.15/24 gw 10.0.2.2 (`kernel/net/net_init.c:149-212`). Renewal/rebind driven by lwIP timers while up; lease-expiry-failover and link-flap re-DHCP untested. Boot state exported via `net_boot_summary` (`net_init.c:71-74`) for one-line console.

## 16. DNS — `[PARTIAL]`

Resolver runs (table 4, servers set statically then overwritten by DHCP offer). Only consumer: `kernel_ping` (`dns_gethostbyname` + 5s wait, `net_ping.c:329-342`). No user resolver API (`resolve("example.com")` impossible from ring-3), no cache tuning, no AAAA (IPv6 off). `[MISSING]` user DNS.

## 17. IPv4 — `[OK]`

Static-or-DHCP single address, default netif route, broadcast flag on. Fragmentation code compiled (ipv4/*) but untested.

## 18. IPv6 — `[MISSING]`

`LWIP_IPV6 0`, `core/ipv6/*` uncompiled, no `AF_INET6`, no ND6/SLAAC/DHCPv6/AAAA.

## 19. Routing — `[PARTIAL]`

Single default interface (`netif_set_default`). No table, no gateway selection logic beyond DHCP/static gw, no multi-interface (singleton netif+driver).

## 20. Loopback — `[MISSING]`

`LWIP_HAVE_LOOPIF 0`. No 127.0.0.1/::1; process-to-process via localhost impossible.

## 21. HTTP / HTTPS / TLS — `[MISSING]`

No http/tls/ssl/crypto refs in kernel/libs/apps/system/drivers/tools (verified grep). No vendored mbedTLS/BearSSL/wolfSSL/OpenSSL.

## 22. User Applications

- `ping <host>` (`system/shell_core.c:436`) → syscall 41 → kernel prints + RTT.
- `nettest <ip> <port>` (`system/shell_core.c:605-640`) → 52→53→54→55→56, IP-literal only, echo-style. Only socket demo in-tree; `apps/` has zero net users (verified grep).
- Both run in the kernel shell task (ring-0 wrappers in `system/shell.c`), so ring-3 path (usercopy bounce) exercised only by spawned ELFs — none exist yet.

## 23. Process Lifecycle / Cleanup — weakest layer

`proc_do_exit` → `vfs_close_all` + event/kwm cleanup (`kernel/proc/proc.c:274-277`); **no `ksock_*` cleanup**. Exiting with open sockets leaks: slot stays non-FREE, pcb + TCP connection persist, callbacks still reference `socks[i]`. On slot reuse (`ksock_socket` picks any `S_FREE` — leaked slots never FREE, so pool shrinks to 0 → permanent `-1` after 8 leaked sockets) and worse: a leaked pcb keeps feeding `cb_recv/cb_err` into a slot a *later* connection may reason about via generation counters (new alloc bumps gen, old callbacks write `s->state/rx` of the slot now owned by another connection → cross-connection state corruption). **Concrete, evidence-backed.** Also: tasks blocked in `sti;hlt` poll loops are RUNNING (not on a wait queue), so `proc_kill`'s BLOCKED path (`unblock_task`, `proc.c:359`) can't wake them — kill converges only at timeout (5–10s) or syscall return. Delayed-kill, not lost-kill (post-check at `syscall.c:139` still fires).

## 24. Security Findings

- `[HIGH]` Cross-task socket use + exit-leak → stale-pcb writes into reused slots (§23). Global namespace, no owner, no cleanup.
- `[MEDIUM]` Kill-during-blocking-syscall delayed up to timeout (§23). Availability, not escape.
- `[MEDIUM]` RX-ring truncation (`rx_push` drops beyond 4K but `tcp_recved(tot_len)` acks all — data loss, not memory-unsafe).
- `[LOW]` Promiscuous UPE+MPE (`e1000.c:365-366`) widens RX to all unicast/multicast; fine for bringup, note for later.
- `[LOW]` Predictable LCG `LWIP_RAND()` (`lwipopts.h:249-255`) → guessable DNS/DHCP XIDs/ports.
- Positives (keep): usercopy bounce + `UC_MAX_*` caps, fd bounds checks, pbuf/IP-header length validation, close-callback detach, gen counters, bounded serial TX in panic (out of scope but adjacent).
- No raw sockets exposed to ring-3 (only kernel ICMP) — privilege model accidentally safe.

## 25. Performance Findings (no optimization done)

2 copies TX (pbuf→stack frame→DMA), 1 copy + pbuf alloc RX, `kmalloc(≤64K)` per send/recv syscall, RX latency floor = timer tick (~16ms @60Hz), all-RX on BSP under global lock, 4K socket RX rings, 16-deep HW rings. No benchmarks exist. Bottleneck order-of-magnitude: tick-rate polling first, copies second, lock third.

## 26. Existing Tests — none for net

`tests/` grep for net/ping/sock/dhcp/dns/tcp/udp/lwip/e1000: zero hits. Coverage = manual `ping`/`nettest` in QEMU + boot-log DHCP line. All rows `[UNTESTED]` formally.

## 27. QEMU Networking

Every run target (`run/run-serial/run-wd/stress/conc/heap-stress/heap-watch`, `Makefile:1916-1981`): `-nic user,model=e1000` (slirp NAT, no TAP/bridge/port-forwarding configured). Guest .15 via DHCP (fallback static), gw .2, DNS forwarded (slirp serves .3; lwIP takes DHCP-provided). `-cpu max`, 1G (512M stress), `-smp 8` (run) / 4. COM1 → `serial.log` (`run`/`run-wd`) or stdio.

## 28. Real Hardware

e1000 covers only Intel 82540EM/82545EM/82574L (devices QEMU emulates; rare in modern laptops). No virtio-net, no Realtek/iwlwifi/MediaTek/USB. Wi-Fi = entire missing stack (802.11 + WPA + drivers), not a "small driver". Real-hardware networking today: `[MISSING]` for all practical purposes.

## 29. Feature Matrix

| Feature | Status | Evidence | Layer | Tested |
| --- | --- | --- | --- | --- |
| PCI NIC detection | [OK] | e1000.c:380-404 (func-0 only) | driver | manual QEMU |
| NIC driver (e1000) | [PARTIAL] | poll-only, IMS dead, no recovery | driver | manual |
| DMA | [PARTIAL] | PMM pages+HHDM, x86-coherent assumption | driver | manual |
| RX | [OK] | e1000_poll→pbuf→ethernet_input | driver/netif | ping/nettest |
| TX | [OK] | low_level_output→e1000_send | netif/driver | ping/DHCP/nettest |
| Ethernet/ARP/IPv4/ICMP | [OK] | lwipopts 1 + glue | lwIP | manual |
| IPv6 | [MISSING] | LWIP_IPV6 0, ipv6/ unbuilt | lwIP | — |
| TCP | [PARTIAL] | client-only raw API | kernel+lwIP | manual nettest |
| UDP | [STUB] | compiled, 0 refs | lwIP only | — |
| DHCP | [OK/PARTIAL] | net_init.c:149-212 | kernel+lwIP | boot log |
| DNS | [PARTIAL] | resolver on, no user API | lwIP+kernel | via ping |
| Routing | [PARTIAL] | single default netif | lwIP | manual |
| Socket API | [PARTIAL] | 52-56 client-only, no POSIX opts | syscall | manual |
| FD integration | [MISSING] | separate namespace, §11 | kernel | — |
| Blocking I/O | [OK] | fixed timeouts 5/5/10s | kernel | manual |
| Nonblocking/poll/select | [MISSING] | no API | — | — |
| Timeout config | [MISSING] | hardcoded | kernel | — |
| TLS/HTTP/HTTPS/NTP | [MISSING] | zero refs | — | — |
| Loopback | [MISSING] | LOOPIF 0 | lwIP | — |
| Wi-Fi | [MISSING] | no stack/driver | — | — |
| Cleanup on exit | [BROKEN] | §23 leak + stale pcb | proc/ksock | — |
| SMP safety | [OK] | net_lock+gen+ping_busy | kernel | SMP boot (stress incidental) |

## 30. Architectural Gaps

1. **Sockets outside FD + no owner + no exit cleanup.** Evidence: §11, §23 (`vfs_fd.c:724`, `proc.c:274`, `net_socket.c:111`). Matters: leak-to-exhaustion + cross-connection corruption + kill delay. Layer: kernel net + proc. Depends on: FD design decision (merge vs parallel table with owner+cleanup).
2. **No server-side TCP / no UDP / no DNS API.** Evidence: §12/13/16. Matters: apps can't serve, datagram, or resolve. Layer: syscall+userlib. Depends on: gap 1 (ownership first, else more leaking objects).
3. **Polling-only RX, dead IMS, never-checked link state.** Evidence: §4. Matters: latency floor, silent link loss, wasted timer work. Layer: driver+timer. Depends on: IDT/IRQ routing for NIC.
4. **Errors collapse to -1.** Evidence: `sys_net.c` (all paths). Matters: apps can't distinguish refused/timeout/OOM. Layer: syscall ABI. Depends on: errno convention decision.
5. **No loopback; single NIC singleton.** Evidence: §19/20. Matters: no localhost IPC, no multi-homing path. Layer: lwIP glue + driver model.

## 31. Future Work Candidates (dependency order, NOT priority)

- Foundation: per-task socket ownership + exit cleanup (+kill-wakeup for hlt-blocked tasks); FD merge-or-own decision; errno/err-code convention; link-state + RX error counters wired to logs.
- Core: UDP sockets; TCP listen/accept; user DNS resolver API; DHCP lifecycle tests (renew/expire/link-flap); IPv6 (needs ipv6/ compiled + ND6 + SLAAC/DHCPv6); loopback netif.
- User-space: libnet-style wrapper (hostname resolve, dial with timeout); `ping` as ring-3 app; diagnostics (`ifconfig`-ish status dump from `g_kyuzen_netif`).
- App protocols: SNTP client (needs time API decision); HTTP/1.1 minimal client (needs TCP+DNS); TLS only after crypto lib decision (none vendored).
- Advanced (deferred, hobby-scale): virtio-net driver (QEMU fast path), IRQ-driven RX, Wi-Fi (full 802.11 stack — explicitly not a driver patch), fine-grained locking.

## 32. Unknowns / Requires Verification (QEMU runs, not code)

- DHCP renew/rebind over long uptime; behavior when slirp offers different DNS.
- TCP behavior under loss (QEMU slirp rarely drops) — retransmit path untested.
- Throughput/latency numbers — no benchmark run.
- `MEMP_NUM_ARP_QUEUE 8` adequacy under ARP storms — untested.
- Exact slirp DNS IP consumed post-ACK (code takes it blindly, `dhcp.c:743`).
