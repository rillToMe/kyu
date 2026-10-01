# Process Model

KyuzenOS has a single authoritative process model: the kernel task structure
(`task_t`) is the truth. There is no separate userspace PID, no shell-only
argv, and no distinct stdio table. Contracts are declared in `include/proc.h`
and `include/task.h`.

## Task Identity

| Concept | Definition |
| --- | --- |
| PID | The task slot id (`task_t.id`), stable while the task is alive, including while a zombie |
| PID reuse | Only after the parent reaps the task (`ZOMBIE` → `DEAD`) |
| Parent | `parent_id`, set once at creation; `PROC_NO_PARENT` (-1) for kernel tasks with no creator |
| Credentials | `uid`/`gid` in `task_t.cred`, inherited from the creator |

`sys_getpid` (70) and `sys_get_task_id` (43) return the PID.
`sys_get_pid` (45) is **not** the PID — it returns a per-address-space cookie for
diagnostics.

Parenthood and credentials are independent: sharing a UID never implies a
parent link, and `waitpid` checks the parent link only.

## Credentials

Identity lives in `task_t.cred` (`include/cred.h`: `uid` and `gid`), never in a
global.

| Rule | Behavior |
| --- | --- |
| Task 0 | Created as `(0, 0)` — kernel/root |
| `create_task` / `create_task_prio` | Inherits the creator's credentials |
| `create_user_task` (spawn) | Inherits the syscall caller's credentials; `-1` if no creator can be determined (never silently root) |
| `sys_set_uid` (27) | Root-only transition of the caller's own `uid`+`gid`; non-root returns `-1` |

Kernel-enforced root-only operations: `fs_format` (5), `shutdown` (38),
`reboot` (39). The shell's `sudo` flag is a UX gate; the kernel check is the
real boundary. KWM window ownership is task-id based and never trusts the UID.

Not a POSIX model: there is no setuid bit, no sudoers, no password hashing in
the kernel, no file ownership/modes, and no `fork/argv/stdio/waitpid/kill`
beyond what is documented below.

## Lifecycle and States

A task transitions through these states:

```text
READY / RUNNING / SLEEPING / BLOCKED / ZOMBIE / DEAD
```

`ZOMBIE` retains the pid, parent, exit status, name, and credentials until the
parent reaps it. The scheduler only selects `READY` tasks.

### Creation

- `create_task` / `create_task_prio` — kernel task with a private kernel stack.
- `create_user_task` — the only Ring-3 task factory, used by spawn paths.
- `task_fork` — duplicates the caller (see below).

A new task is placed on the least-loaded CPU and published to the run queue
only once it is fully built.

### Exit

Every death converges on one function, `proc_transition_locked()` in
`kernel/proc/proc.c`, reached through either:

- `proc_exit()` — normal self-exit (spawned tasks via `sys_exit` 34).
- `proc_exit_kill()` — the kill convention.

There is no second cleanup implementation. `proc_exit()` is guarded so a racing
transition (already `ZOMBIE`/`DEAD`) performs no second cleanup.

Cleanup fans out from the single path: close file descriptors
(`vfs_close_all`), flush the event queue, destroy the task's KWM windows,
destroy the address space, free the stack and user heap. Then the task becomes
`ZOMBIE` (if a live non-reaper parent exists) or `DEAD` (orphan). Termination
and reclamation are separate: the slot is reusable only after reap.

## argv ABI

`main(int argc, char **argv)` receives arguments in registers:

- `RDI = argc`
- `RSI = argv`

`argv[argc] == NULL`; `argv[0]` is the application name. Applications with the
old `void main()` signature simply ignore `RDI`/`RSI` and keep working.

Bounds:

| Limit | Value |
| --- | --- |
| `argc` range | 1..16 (`PROC_MAX_ARGC`) |
| Each argument | ≤ 64 bytes including NUL (`PROC_MAX_ARG_LEN`) |
| Total argument bytes | ≤ 512 (`PROC_ARG_TOTAL_MAX`) |

The kernel validates everything; over-long input fails the spawn (`-1`) rather
than overflowing. The `argv` strings and pointer array live on the child's own
user stack, below `USER_STACK_TOP`, 16-byte aligned.

- `start app` → `argc=1`. `start app a1 a2` → `argc=3`.
- `sys_spawn(path)` → `argc=1`, `argv[0]` = basename of `path`.
- `sys_spawn_argv(path, argc, argv)` (68) → full argv.

No environment variables and no `PATH` lookup; only the `/apps/` prefix is
resolved.

## Spawning

- `sys_spawn` (57) — spawn with a single argument (basename).
- `sys_spawn_argv` (68) — spawn with full argv.
- `sys_spawn_redir` (77) — spawn with explicit fd redirection
  (`spawn_stdio_t`: caller fds for the child's 0/1/2; `-1` = fresh TTY).
  Installed after slot assignment, before the child is schedulable.

The single-image ELF loader `exec_load_image` is shared by spawn and execve.

## exec

- `sys_exec` (33) — **legacy, retained**. Self-replacement that intentionally
  destroys the caller's windows (used by the launcher). It is non-atomic
  (destroys the address space before loading the new image) but touches only
  `self`, so it cannot corrupt the lifecycle. New code should use 78 + 79.
- `sys_execve` (79) — **canonical, atomic in-place image replace**: build the
  new address space first, then swap address space / heap / argv / trap frame
  and destroy the old one. Same pid/ppid/credentials/fds/windows; fresh
  stack/entry/argv; general-purpose registers zeroed. On failure it returns
  `-1` with the old image untouched.

## fork

`sys_fork` (78) duplicates the caller. The parent receives the child PID; the
child resumes after the fork trap with `0`; failure returns `-1`.

- **Strategy: full physical copy, not COW.** See
  [Memory Management](memory.md) for why COW was rejected.
- **Context**: the child's kernel stack carries a verbatim copy of the parent's
  syscall trap frame with `rax` forced to `0`. The kernel stack is never shared.
  No FPU/SSE context exists anywhere (`-mno-sse`), so none is cloned.
- **FDs**: `vfs_fork_inherit` copies the whole fd table at the same numbers
  onto the same open descriptions. The child table must be empty or fork fails
  without clobbering.
- **Heap**: `uheap_clone` duplicates the region list; pages ride along in the
  address-space clone. Parent and child allocate independently afterwards.
- **KWM**: the child starts with zero windows (ownership is per-task).
- **Kernel contexts** (no user address space, e.g. Task 0/idle) are rejected
  with `-1`; Ring-3 is required.

## exit and wait

- `sys_exit` (34) takes the exit code in `RBX`. A plain `sys_exit()` exits 0;
  `sys_exit_code(n)` sets it explicitly. A syscall returning `-1` is not an
  exit code — only this path records `exit_code`.
- `sys_waitpid` (69): `waitpid(pid, &status, 0)`. Parent-only (non-child →
  `-1`). Blocks with the standard wait-queue primitives (no polling). A child
  exit on any CPU wakes a parent on any CPU. `options` must be 0
  (`PROC_WNOHANG` exists but the shell uses explicit waits). `waitpid(-1,...)`
  reaps any child.
- **Orphans**: children are reparented to Task 0 (the kernel reaper
  placeholder) at parent exit. Task 0 never waits, so its adoptees auto-reap
  (`DEAD`, no zombie accumulation).
- **init (PID 1)**: `init.elf` is the first ring-3 process. The kernel loads
  it in `boot_handoff_to_init()` and then idles — it contains no user code.
  `init` spawns `login.elf` and supervises it: a dead child is logged to
  serial and restarted with exponential backoff (1s → 30s cap). `login` in
  turn spawns `desktop.elf` and `shell.elf` after authentication. See
  [Ring-3 Init Migration](../design/ring3-init-migration.md).
- `sys_proc_list` (72) returns a read-only `proc_info_t` snapshot
  (`pid/ppid/state/uid/gid/exit_code/name`) for Task Manager. Userspace cannot
  mutate task state through it.

### `proc_info_t` layout

```c
typedef struct {
    int32_t  pid;
    int32_t  ppid;
    uint8_t  state;
    uint8_t  exit_reason;
    uint16_t _pad1;
    uint32_t uid;
    uint32_t gid;
    int32_t  exit_code;
    char     name[16];
} proc_info_t;
```

## Termination and Kill

### Kill model

`sys_kill(pid)` (73) returns `0` on success or `-1` if denied. This is **not**
POSIX signals: there are no signal numbers, handlers, groups, or sessions.
A killed child reports the KyuzenOS convention:

- `exit_code == PROC_KILL_EXIT_CODE` (125)
- `exit_reason == PROC_EXIT_KILLED`

Normal exits keep their plain code with `PROC_EXIT_NORMAL`. `waitpid` callers
compare with `==`; `proc_list` exposes the authoritative `exit_reason`.

### Authorization (`proc_can_kill`)

Kernel-enforced; userspace UID is never trusted.

| Caller | May kill |
| --- | --- |
| Normal user | Own child only (`target.parent_id == caller`) |
| Root | Any `TASK_KIND_SPAWNED` task |
| Suicide (`pid == self`) | Allowed for spawned tasks; exits directly |

Always denied: PID 0, any `TASK_KIND_KERNEL` task (even for root), out-of-range
PIDs, and `DEAD`/`ZOMBIE` slots. A second kill fails (never double-cleans); a
double kill while pending returns `0` (idempotent).

### Synchronous vs asynchronous termination

| Target state | Behavior |
| --- | --- |
| `READY` | Synchronous: purged from every run queue, then transition + cleanup run in the killer's context |
| `BLOCKED` / `SLEEPING` | Asynchronous: `kill_pending` set, then `unblock_task` wakes it; it self-removes and calls `proc_exit_kill()` |
| `RUNNING` (incl. another CPU) | Asynchronous: flag + reschedule IPI; the target observes at its next safe boundary |
| Pure CPU-bound | Observed at the next timer preemption (`proc_observe_kill_sched`) — no syscall required |

`proc_observe_kill_sched` runs at the very top of `schedule_on_cpu`, before any
scheduler lock, on the preempted task's own stack. The gate is cheap: the
victim must be in Ring 3 (`regs->cs & 3 == 3`), be this CPU's current
`TASK_RUNNING` spawned task, and have `kill_pending` set. The full exit then
runs in the target's own context with no cross-CPU frees.

## File Descriptors

Each task has its own fd table (`kernel/fs/vfs_fd.c`), VFS-backed and sharing
the same table/locks/owner rules as files. See
[Filesystem](../filesystem/README.md) for the fd model, pipes, and dup/dup2.

At creation each task gets fd 0/1/2 (`stdin` read-only, `stdout`/`stderr`
write-only), each with its own open description, all routed to the shared
console TTY. `read(0)`/`write(1)`/`write(2)` work via syscalls 48/49.

## Shell Job Control

The shell (`system/shell_core.c`) provides explicit opt-in builtins:

- `jobs` — read-only list of own children.
- `reap <pid>` — blocking `waitpid` on one child, prints status
  (`(killed)` for `PROC_KILL_EXIT_CODE`).
- `kill <pid>` — `sys_kill` wrapper.

`start` remains asynchronous. See [Shell & CLI](../userspace/shell.md).

## Not Implemented

Signals (including `SIGPIPE` — a broken-pipe write fails with `-1`), signal
handlers, process groups/sessions, PTY/TTY per-terminal routing, job control
(`&`), `2>` stderr redirection, environment variables, per-service supervision
beyond init's single-child restart policy, whole-table fd inheritance on spawn (only
explicit 0/1/2 via `sys_spawn_redir`), COW/shared memory/`mmap`/ASLR, and
`vfork`.

## Related Documentation

- [Scheduler](scheduler.md) — run queues and context switching
- [Synchronization](synchronization.md) — the blocking primitives wait uses
- [Syscalls](syscalls.md) — boundary copy contract per syscall
- [Userspace Model](../userspace/overview.md) — Ring-3 isolation
