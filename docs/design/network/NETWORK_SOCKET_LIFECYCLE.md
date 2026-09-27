# KyuzenOS Network Socket Lifecycle & Ownership

Source of truth: `kernel/net/net_socket.c`, `include/net_socket.h`,
`kernel/syscall/sys_net.c`, `kernel/proc/proc.c` (cleanup call sites),
`kernel/syscall/sys_proc.c` (exec reown), `tests/host/unit/netsock_test.c`.

Fractional references below name the file; line numbers drift — grep the symbol.

## Ownership

Every non-FREE socket has exactly one owner: `(owner_pid, owner_cookie)`.

- `owner_pid` = task slot id (same PID as `task_t.id`, stable while alive).
- `owner_cookie` = per-address-space id from `as_cookie_next()`
  (`kernel/syscall/sys_proc.c`), stored per-task (`task_t.cookie`), renewed
  on every exec (`sys_load_elf` 25, `sys_exec` 33, `sys_execve` 79).

The pair is recorded at `ksock_socket()` from the trapping task
(`sys_net_handle` passes `st->id`/`st->cookie`) and re-validated on EVERY
subsequent op (`resolve()`). The cookie closes the PID-reuse alias: a task
that reuses a dead owner's slot always gets a fresh cookie and can never
match the old pair. No task context (`st == NULL`, idle/early boot) owns
nothing — socket creation fails with `KSOCK_EOWNER`.

There are **no kernel-owned sockets** (nothing in the kernel holds a TCP
socket today). `owner = nobody` does not exist.

FD integration: **Option B** (separate tables) is kept. Sockets stay outside
the VFS fd table (no dup/inherit, invisible to `fork` — the child shares fds
but not sockets). Rationale: merging into VFS would be a large rewrite for
no lifecycle gain; explicit ownership + exit cleanup gives the same safety.
Migration to fd-backed sockets remains possible later (would need
`vfs_close_all` to route socket entries into `ksock_close`).

Exec: sockets survive exec like fds. The exec paths renew the task cookie,
so each calls `net_task_reown(pid, new_cookie)` right after — without this
the owner's own sockets would orphan (cookie mismatch, leaked until exit).

## Handle

Opaque `int`, **not** a slot index:

```text
handle = (generation << KSOCK_HANDLE_SHIFT) | slot     // SHIFT = 3, MAX = 8
```

Generation starts at 1, bumps on every allocation AND every free, never 0.
`resolve()` rejects: `h <= 0`, slot out of range, `state == FREE`,
generation mismatch, owner mismatch. A stale handle (closed socket, reused
slot) therefore fails with `KSOCK_EHANDLE` and can never address the new
connection — including wait loops, which re-resolve each quantum.

## Generation

Per-slot `gen` counter, mirrored into the callback-context ring (below).
Invalid values (`<= 0`) never validate. Wrap-around reuses `1`, which is
safe: validity always requires equality with the LIVE generation, and a
wrap would need 2^31 same-slot allocations within one connection lifetime.

## Lifetime

```text
FREE -> ALLOC -> CONNECTING -> CONNECTED -> FREE
                          \-> ERR -> FREE   (refused/timeout/abort/peer-RST)
```

- `ALLOC`: pcb created, callbacks registered, owner recorded.
- `CONNECTING`: `tcp_connect` issued; `cb_connected` resolves to
  `CONNECTED`/`ERR`. Connect-timeout aborts the pcb (detached first) and
  dishonors to `ERR` — no stuck CONNECTING sockets.
- `CONNECTED`: send/recv/close valid. `cb_err` (RST/abort by peer) moves to
  `ERR` with `pcb == NULL`; queued RX survives for draining.
- `ERR`: terminal dishonor; still closable/cleanable, never usable for I/O.
- No `CLOSING` state: close is synchronous under `net_lock`, so no async
  close exists. `S_CLOSED` enumerator is accepted by recv's state check for
  forward compatibility but nothing sets it today.

Close order (`close_locked`, always under `net_lock`):

```text
detach PCB callbacks (arg/recv/err -> NULL: no new callback access)
  -> tcp_close(), fallback tcp_abort()
  -> free RX pbuf queue (unacked bytes die with the dead conn; peer gets RST)
  -> state FREE, owner cleared, waiter cleared, generation bumped
  -> slot reusable
```

## PCB

`tcp_arg(pcb)` receives `&ctxs[slot][gen % CTX_RING]` — a per-allocation,
then immutable, snapshot `{slot, gen, pcb}` in static storage (never freed,
never dereferenced after free). Every callback (`cb_recv`, `cb_connected`,
`cb_err`) runs `cb_slot(arg)` first:

```text
arg in ctxs range + slot consistent + socket non-FREE
  + ctx.gen == live gen + ctx.pcb == live pcb
  -> valid; else drop (pbufs freed WITHOUT tcp_recved, FIN/err ignored)
```

`cb_recv`/`cb_connected` additionally check `s->pcb == pcb`. `cb_err` cannot
(lwIP passes no pcb) — the ctx gate is its protection. A stale pcb (TIME-WAIT
retransmit, RST for the old connection, detached-close leftovers) therefore
resolves to *dropped*, never to the new owner's state. The ring (4 entries
per slot, not 1) is what keeps the snapshot immutable: overwriting the entry
a stale pcb points at would destroy the evidence.

Residual, accepted by design: full alias needs CTX_RING same-slot
reallocations AND a recycled pcb address AND a callback the detach protocol
should have suppressed. The detach protocol (all detaches under the same
`net_lock` that all callback dispatch holds) makes that last leg impossible:
a detached pcb can never fire again.

## Cleanup

`net_process_cleanup(pid)` — idempotent, outside all locks, takes `net_lock`
internally. For every non-FREE socket with `owner_pid == pid`: full
`close_locked()` (detach, close/abort, free RX, bump gen).

Call sites (`kernel/proc/proc.c`):

- `proc_do_exit()` — after `vfs_close_all`, before the TCB transition.
  Covers: normal exit, exit syscall, fatal crash, forced termination, and
  kill convergence (all terminate through `proc_do_exit`).
- `proc_kill()` synchronous-READY path — after `vfs_close_all`, before the
  remote transition. The async (BLOCKED/SLEEPING/RUNNING) path needs no
  separate call: the target converges on `proc_exit_kill()` → `proc_do_exit()`.

Lock discipline: `vfs_close_all` (vfs lock) and `net_process_cleanup`
(net_lock) run sequentially, never nested. Direction stays
`proc -> net -> lwIP`; the net layer never touches scheduler/VFS internals
(except `unblock_task`, see Concurrency).

## Kill

Blocking socket ops sleep in `TASK_SLEEPING` via `task_sleep_ms()` quanta
(`KSOCK_WAIT_QUANTUM_MS` = 20 ms), registered as the socket's `waiter_tid`
under `net_lock` before each quantum. `task_sleep_ms()` observes
`kill_pending` and converges noreturn on `proc_exit_kill()`; `proc_kill()`
already wakes BLOCKED/SLEEPING tasks via `unblock_task()`. No
network-specific scheduler mechanism was added — the existing generic
primitives (`task_sleep_ms` + `unblock_task` + `kill_pending`) are reused.

Callbacks wake the waiter early through `net_wake_waiter()` →
`unblock_task(waiter_tid)` (ISR-safe, no-op if not blocked). Every wait loop
additionally has an absolute deadline (connect/send 5 s, recv 10 s), so a
missed wake costs latency, never a hang. A killed waiter never returns into
socket code; its sockets are released by the exit-path cleanup above, which
also clears `waiter_tid`.

Wait conditions:

```text
connect: CONNECTED / ERR / handle-invalidated / timeout / killed
send:    sndbuf space / partial progress resets deadline / closed / timeout / killed
recv:    rx_bytes > 0 / FIN-drained / ERR / handle-invalidated / timeout / killed
```

`sti;hlt` busy-polling is gone from the socket layer (one `KSOCK_STI()` at
op entry re-enables the timer poll that drives lwIP; the actual waiting is
scheduler sleeping).

## RX

Received payload is owned as queued lwIP `pbuf` chains (`rx_head/tail`,
`rx_bytes`) — no copy, no fixed 4K ring. `tcp_recved()` is called ONLY for
bytes `rx_consume()` hands to `recv()` (in `u16_t` pieces), i.e. **only
acked on delivery to userspace**.

- Under `KSOCK_RX_LIMIT` (32 KiB/socket): pbuf queued UNACKED. Un-recved
  bytes keep lwIP's receive window shrunk → natural backpressure, the peer
  slows down by itself.
- Over the limit: pbuf chain freed WITHOUT `tcp_recved` → the peer
  retransmits later. Nothing acknowledged is ever discarded (the old bug:
  ring-full truncation after `tcp_recved(tot_len)`).
- Memory is doubly bounded: the cap plus the lwIP pbuf pool itself.
- `pbuf_free` releases one pbuf only — chain frees always walk
  (`pbuf_free_chain`); partial consume slides `payload/len/tot_len`.

## Concurrency

One coarse `net_lock` (irqsave spinlock) serializes ALL lwIP entry: socket
syscalls on any CPU + `cb_network` poll + cleanup paths. Callbacks assume it
held. Lock order, the only nesting in the subsystem:

```text
net_lock -> scheduler_lock   (via unblock_task in net_wake_waiter)
```

No path takes `net_lock` while holding `scheduler_lock`, any wait-queue
lock, or the VFS lock. `wait_quantum()` never holds `net_lock` across the
sleep. Fine-grained locking is explicitly deferred; the coarse lock is kept
for correctness. `ping_busy` behavior unchanged.

## Error ABI

Negative `KSOCK_*` codes (`include/net_socket.h`), all `< 0` so legacy
`n < 0` / `!= 0` checks keep failing safe. `-1` (`KSOCK_ERR`) stays the
generic failure. New, distinguishable:

```text
-2 EHANDLE  bad slot / stale generation / wrong state
-3 EOWNER   caller is not the (pid, cookie) owner (incl. no task context)
-4 ETIMEOUT deadline expired (connect/send/recv)
-5 ECONN    refused / handshake failed / write protocol error
-6 ECLOSED  peer closed / connection errored (send path; recv uses 0-on-drain)
-7 ENOMEM   table full / pcb alloc failed / bounce OOM
```

`recv` keeps `0` = peer-FIN-drained (unchanged). No POSIX errno numbers are
introduced; userlib passes codes through opaquely.

## TX

Unchanged semantics, now documented: `tcp_write(COPY)` + `tcp_output` with
`ERR_MEM` → quantum-retry to a 5 s rolling deadline. An `e1000` TX-ring-full
drop surfaces as `ERR_IF` from `low_level_output` (`kyuzen_netif.c`) — lwIP
keeps the segment queued and retransmits on its own timers. TCP therefore
cannot silently lose data on TX-full; no driver change was needed.

## Invariants (non-negotiable)

1. A socket has exactly one owner (`pid+cookie`); no ownerless sockets exist.
2. A process cannot use/close another process's socket (`EOWNER`).
3. A stale handle cannot address a reused slot (`EHANDLE` via generation).
4. A closed socket cannot receive new lwIP callbacks (detach + ctx gate).
5. No live PCB callback can reference a freed object (static storage only;
   generation + pcb-snapshot gate; detach-before-close ordering).
6. Process exit releases all owned sockets (table capacity recovers).
7. Process kill releases all owned sockets (sync path + exit convergence).
8. A blocked network syscall is kill-interruptible (SLEEPING + quanta +
   deadlines; no `sti;hlt` poll loops).
9. TCP data is never acked before userspace consumes it (consume-then-recved;
   over-limit drops stay unacked for retransmit).
10. RX queue is bounded (`KSOCK_RX_LIMIT` + pbuf pool; window backpressure).
11. All lifecycle operations are SMP-safe under `net_lock` (+ documented
    `net_lock -> scheduler_lock` order).
12. User-pointer discipline unchanged (bounce + `user_range_ok`, copy-out
    after return, no ring-3 deref in the net layer).
13. No unrelated networking features (no UDP/server/DNS/IPv6/drivers/TLS).

## Deferred (intentionally NOT done)

UDP/TCP-server/DNS user API, IPv6, Wi-Fi, virtio-net, HTTP/TLS/NTP, network
namespaces, routing, `e1000_link_up()` wiring (exists, never called —
link-down still undetectable), `LWIP_RAND()` LCG predictability, VFS-fd merge
(Option A migration notes above), fine-grained locking, full POSIX errno.
