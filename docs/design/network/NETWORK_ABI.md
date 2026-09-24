# KyuzenOS Network ABI (audited, not specified)

Source of truth: `kernel/syscall/syscall.c:66-72` (registers), `:111-114` (routing), `kernel/syscall/sys_net.c` (semantics), `libs/core/userlib.c:316-323,476-507` (ring-3 wrappers), `system/shell.c:35-58` (ring-0 wrappers), `include/usercopy.h:22` + `UC_MAX_SOCK` (caps).

Trap: `int 0x80`. `RAX` = number, `RBX`/`RCX`/`RDX` = args 1–3. All returns are `int64`-sign-extended; `-1` = any failure (no errno).

| # | Name | Args | Returns | Notes |
| --- | --- | --- | --- | --- |
| 41 | ping | RBX=`const char*` host (≤127+NUL, `UC_MAX_HOST`) | avg RTT ms ≥0, `-1` fail | kernel sends 4×ICMP echo, resolves via lwIP DNS (5s), prints to TTY itself |
| 52 | socket | — | 0–7 sockfd (NOT a VFS fd), `-1` | TCP pcb; global namespace, no owner |
| 53 | connect | RBX=s, RCX=IPv4 BE, RDX=port | 0 / `-1` | blocking ≤5s, IP-literal only (no DNS) |
| 54 | send | RBX=s, RCX=buf, RDX=len ≤64K | bytes (partial ok) / `-1` | copy-in to kmalloc bounce; ≤5s per call |
| 55 | recv | RBX=s, RCX=buf, RDX=len ≤64K (clamped) | bytes / 0 peer-FIN / `-1` | blocking ≤10s; copy-out after return |
| 56 | sock_close | RBX=s | 0 / `-1` | `tcp_close`, fallback `tcp_abort`; bumps slot generation |

Deliberately absent: bind/listen/accept, sendto/recvfrom, shutdown, sockopt, nonblock/poll/select, DNS resolve, anything UDP/IPv6. Socket ids must NOT be passed to fd syscalls (47-51,74-76) and vice versa — separate tables, no cross-checks.
