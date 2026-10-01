# KyuzenOS Network ABI (audited, not specified)

Source of truth: `kernel/syscall/syscall.c:66-72` (registers), `:111-114` (routing), `kernel/syscall/sys_net.c` (semantics), `libs/core/userlib.c:316-323,476-507` (wrappers — the only implementation), `include/usercopy.h:22` + `UC_MAX_SOCK` (caps).

Trap: `int 0x80`. `RAX` = number, `RBX`/`RCX`/`RDX` = args 1–3. All returns are `int64`-sign-extended. Failures are negative `KSOCK_*` codes (`include/net_socket.h`): `-1` generic, `-2` stale handle, `-3` not owner, `-4` timeout, `-5` conn failed, `-6` closed, `-7` no resource. Legacy `< 0` checks keep failing safe.

| # | Name | Args | Returns | Notes |
| --- | --- | --- | --- | --- |
| 41 | ping | RBX=`const char*` host (≤127+NUL, `UC_MAX_HOST`) | avg RTT ms ≥0, `-1` fail | kernel sends 4×ICMP echo, resolves via lwIP DNS (5s), prints to TTY itself |
| 52 | socket | — | opaque handle ≥0 (generation-tagged, NOT a slot index), negative `KSOCK_*` | TCP pcb; exactly one owner = calling task `(pid, cookie)` |
| 53 | connect | RBX=h, RCX=IPv4 BE, RDX=port | 0 / negative `KSOCK_*` | blocking ≤5s, IP-literal only (no DNS); owner-checked; kill-interruptible |
| 54 | send | RBX=h, RCX=buf, RDX=len ≤64K | bytes (partial ok) / negative `KSOCK_*` | copy-in to kmalloc bounce; 5s rolling deadline; owner-checked |
| 55 | recv | RBX=h, RCX=buf, RDX=len ≤64K (clamped) | bytes / 0 peer-FIN / negative `KSOCK_*` | blocking ≤10s, kill-interruptible; copy-out after return; owner-checked |
| 56 | sock_close | RBX=h | 0 / negative `KSOCK_*` | detach callbacks → `tcp_close`/`abort` → free RX → bump generation; owner-checked |

Every socket op validates `(owner_pid, owner_cookie)` + handle generation: another task's handle (or a stale handle for a reused slot) is rejected, never aliased. Process exit/kill releases all owned sockets (`net_process_cleanup`); exec keeps sockets via `net_task_reown`.

Deliberately absent: bind/listen/accept, sendto/recvfrom, shutdown, sockopt, nonblock/poll/select, DNS resolve, anything UDP/IPv6. Socket ids must NOT be passed to fd syscalls (47-51,74-76) and vice versa — separate tables, no cross-checks.
