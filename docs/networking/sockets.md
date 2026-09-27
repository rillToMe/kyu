# Socket Model

KyuzenOS sockets are kernel-side objects owned by the process that created
them. The socket layer (`kernel/net/net_socket.c`) provides handle-based access
through syscalls 52–56.

## Ownership

A socket is associated with an owning process through an **ownership identity**
recorded at creation. This prevents one process from operating on another
process's sockets:

- The owner is captured from the creating process.
- Every subsequent operation (`connect`, `send`, `recv`, `close`) verifies that
  the caller matches the owner; a mismatch returns `KSOCK_EOWNER` (-3).
- When a process exits, its sockets are closed and their slots released.

## Handles

Sockets are addressed by a small integer handle rather than a raw pointer, so
userspace never sees kernel addresses. A handle identifies a slot in a
fixed-size socket table.

- An invalid handle returns `KSOCK_EHANDLE` (-2).
- Slots are reused after a socket is closed.

### Generation counters

Because slots are reused, a handle alone is not enough to identify a socket
across time. Each slot carries a **generation** counter that is incremented
whenever the slot is allocated or freed. A blocking operation
(`connect`/`send`/`recv`) that is polling a slot captures the generation and
aborts if the generation changes — so a closed-and-reused slot cannot be
mistaken for the original socket.

This closes a class of bugs where a blocking operation would otherwise observe
the state of a *different* socket that happened to reuse the same slot.

## Blocking Model

Operations block using the scheduler's wait primitives (two-phase block; see
[Synchronization](../kernel/synchronization.md)), not busy-waiting:

- `connect` waits for the connection to establish or time out.
- `send` waits for TX buffer space.
- `recv` waits for data or EOF.

A kill delivered while blocked is observed through the same mechanism used by
`waitpid` and mutexes, so the blocked task exits cleanly and its temporary
references are released.

## Lifecycle

```text
socket()      → allocate slot, record owner, return handle
connect()     → establish TCP connection (blocking)
send()/recv() → transfer data (blocking)
close()       → release slot (or process exit)
```

On process exit, the kernel closes all of the process's sockets, so a process
crash or kill does not leak sockets. A dedicated test (`netsock_test`) verifies
close/reuse, exit cleanup, cross-process denial, stale-PCB handling,
kill-while-waiting, RX flood, timeouts, exec re-ownership, and the TCP client
path.

## RX Flow Control

Received data is delivered into the socket's buffer. The layer handles an RX
flood by applying flow control (not delivering unboundedly ahead of the
consumer) so a fast sender cannot exhaust kernel memory.

## Socket State

A socket slot has a state field (for example, free / connecting / connected /
closed). Blocking operations poll this state and abort on generation change.

## Error Codes

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

- **TCP client only.** No `listen`/`accept`.
- **No UDP socket API.**
- **No socket options.**
- **A single global `net_lock`** serializes all socket and lwIP access.

## Related Documentation

- [Networking Overview](README.md)
- [Network Stack](stack.md)
- [Synchronization](../kernel/synchronization.md) — blocking primitives
- [Syscall Table](../reference/syscalls.md)
