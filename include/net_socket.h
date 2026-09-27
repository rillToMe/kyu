#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <stdint.h>

// TCP client socket API over lwIP raw callbacks. NO_SYS=1: all lwIP
// entry is serialized by an internal net_lock; blocking calls sleep in
// TASK_SLEEPING via task_sleep_ms() quanta (kill-interruptible, woken
// early by socket events), so the BSP timer poll can still deliver packets.
//
// OWNERSHIP: every non-FREE socket has exactly one owner task, identified
// by (owner_pid, owner_cookie). pid = task slot id; cookie = per-address-
// space id from as_cookie_next() (sys_proc.c), renewed on every exec.
// The cookie closes the PID-reuse alias: a task that reuses a dead owner's
// slot always gets a fresh cookie and can never match the old owner pair.
// There are no kernel-owned sockets: all sockets are task-owned.
//
// HANDLE: ksock_socket() returns an opaque handle, NOT a slot index:
//   handle = (generation << KSOCK_HANDLE_SHIFT) | slot
// A stale handle (closed socket, reused slot) decodes to a generation
// mismatch and is rejected. Handles must never be passed to VFS fd
// syscalls and vice versa (separate tables).
//
// LIFETIME: FREE -> ALLOC -> CONNECTING -> CONNECTED -> FREE
// (ERR is a terminal dishonor state, freed via close/cleanup -> FREE).
// No CLOSING state: close is synchronous under net_lock (detach callbacks,
// tcp_close/abort, free RX queue, bump generation) so no async close exists.
//
// RX: received TCP payload is owned as queued lwIP pbufs (no copy, no
// fixed ring). tcp_recved() is called ONLY for bytes consumed by recv().
// Data that does not fit KSOCK_RX_LIMIT is freed WITHOUT tcp_recved:
// the sender retransmits later; nothing acknowledged is ever discarded.
// Backpressure is lwIP's own receive window (un-recved bytes shrink it).
//
// ERROR ABI: all failures are negative (< 0, so legacy `n < 0` checks keep
// failing safe). -1 stays the generic error; specific codes below let
// callers distinguish handle/owner/timeout/close/resource failures.

#define KSOCK_MAX            8
#define KSOCK_RX_LIMIT       (32u * 1024u)  // per-socket queued RX cap (bytes)
#define KSOCK_HANDLE_SHIFT   3              // low 3 bits = slot (KSOCK_MAX == 8)
#define KSOCK_WAIT_QUANTUM_MS 20u           // sleep slice for blocking ops

// Error codes (negative; 0/success and >0 byte counts unchanged).
#define KSOCK_OK        0
#define KSOCK_ERR      -1   // generic failure (preserves old -1 contract)
#define KSOCK_EHANDLE  -2   // bad slot, stale generation, or wrong state
#define KSOCK_EOWNER   -3   // caller does not own the socket
#define KSOCK_ETIMEOUT -4   // deadline expired (connect/send/recv)
#define KSOCK_ECONN    -5   // connect refused/failed/aborted by peer or net
#define KSOCK_ECLOSED  -6   // peer closed (recv drained) or socket errored
#define KSOCK_ENOMEM   -7   // no free slot, pcb alloc failed, bounce OOM

// caller identity: pass the trapping task's id + cookie (task_t* st in
// sys_net_handle). No socket exists without an owner; a negative pid
// (no task context) is rejected.
int ksock_socket(int caller_pid, uint32_t caller_cookie);
int ksock_connect(int h, uint32_t ip_be, uint16_t port,
                  int caller_pid, uint32_t caller_cookie);
int ksock_send(int h, const void *buf, uint32_t len,
               int caller_pid, uint32_t caller_cookie);
int ksock_recv(int h, void *buf, uint32_t len,
               int caller_pid, uint32_t caller_cookie);
int ksock_close(int h, int caller_pid, uint32_t caller_cookie);

void ksock_init(void);

// Process lifecycle interface (called by kernel/proc/proc.c, never by
// lwIP callbacks). Outside all locks; takes net_lock internally.
// Cleanup: synchronously closes every socket owned by pid (detach PCB,
// abort connection, free RX queue, bump generations). Idempotent.
// Reown: refreshes owner_cookie after an exec renewed the task's AS
// cookie (sockets survive exec like fds; without this they would orphan).
void net_process_cleanup(int pid);
void net_task_reown(int pid, uint32_t new_cookie);

// Grab the net_lock around e1000_poll + sys_check_timeouts. Called by cb_network.
void net_lock_acquire(uint64_t* saved);
void net_lock_release(uint64_t saved);

// Minimal accounting for lifecycle debugging (kprint-based, no alloc).
void ksock_debug_dump(void);

#endif // NET_SOCKET_H
