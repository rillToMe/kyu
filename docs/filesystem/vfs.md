# VFS & File Descriptors

KyuzenOS exposes files through a POSIX-style file-descriptor (fd) layer in
`kernel/fs/vfs_fd.c`, with the vnode interface (`include/vnode.h`) underneath.

## Layers

```text
userspace syscalls (47–51, 74–76, 81)
        │
        ▼
vfs_fd.c   — fd table, open descriptions, pipes, dup/dup2
        │
        ▼
vnode interface (kzfs_v4_vnode_ops)
        │
        ▼
KyuzenFS (inode cache + extent engine + block cache)
```

## File Descriptors

Each task has its own fd table with `VFS_MAX_FDS` = 16 entries. An fd entry
points to an **open description** — a reference-counted heap object owning the
file position, flags, and (for pipes) the pipe state.

```c
typedef struct vfs_open_file {
    volatile uint32_t refcount;
    uint8_t           kind;
    char              path[23];      // VFS_MAX_PATH
    struct vnode     *vnode;
    uint32_t          pos;
    uint32_t          flags;
    vfs_pipe_t       *pipe;
} vfs_open_file_t;
```

| Constant | Value |
| --- | --- |
| `VFS_MAX_FDS` | 16 |
| `VFS_MAX_PATH` | 23 |
| `VFS_PIPE_CAP` | 4096 |

### Open kinds

| Kind | Value |
| --- | --- |
| `VFS_KIND_FILE` | 0 |
| `VFS_KIND_TTY` | 1 |
| `VFS_KIND_PIPE_READ` | 2 |
| `VFS_KIND_PIPE_WRITE` | 3 |
| `VFS_KIND_DIR` | 4 |

### Open flags

| Flag | Value |
| --- | --- |
| `VFS_O_RDONLY` | `0x0` |
| `VFS_O_WRONLY` | `0x1` |
| `VFS_O_RDWR` | `0x2` |
| `VFS_O_CREAT` | `0x4` |
| `VFS_O_TRUNC` | `0x8` |
| `VFS_O_APPEND` | `0x10` |

Seek origins: `VFS_SEEK_SET` (0), `VFS_SEEK_CUR` (1), `VFS_SEEK_END` (2).

## Standard File Descriptors

At task creation, `vfs_task_init` installs three **separate** TTY descriptions:

| fd | Description | Mode |
| --- | --- | --- |
| 0 | stdin | `O_RDONLY` |
| 1 | stdout | `O_WRONLY` |
| 2 | stderr | `O_WRONLY` |

All three route to the shared console TTY, but each has its own open
description because sharing one would merge the permission flags. `read(0)` and
`write(1)`/`write(2)` work through syscalls 48/49.

## dup / dup2

- `sys_dup` (74): returns the lowest free fd on the **same** open description
  (one shared offset, one shared buffer). All kinds are duplicable.
- `sys_dup2` (75): points `newfd` at the same description as `oldfd`.
  `oldfd == newfd` is a validated no-op. An open `newfd` is closed first; the
  new reference is acquired before the target is released, so self-sharing
  cannot destroy the description.

A read on a duplicate advances the single shared offset. `close` drops one
reference; only the last close flushes (dirty files) and frees. `proc_exit`
releases each fd the same way, so one task's exit never invalidates another
task's references.

## Pipes

A pipe is one `vfs_pipe_t` — a bounded circular buffer of `VFS_PIPE_CAP` (4096)
bytes — shared by exactly one `PIPE_READ` and one `PIPE_WRITE` description.

```c
typedef struct {
    wait_queue_t wq;
    uint8_t     *buf;
    uint32_t     rpos, wpos, used;
    uint8_t      readers, writers;   // side-alive flags
} vfs_pipe_t;
```

### Semantics

- `sys_pipe` (76): `fds[0]` = read end, `fds[1]` = write end, both in the
  caller's table. Both-or-neither: if the table is full, nothing is allocated.
- **Blocking**: empty with live writers → reader blocks; full with live readers
  → writer blocks. Wake-all on every state change, with the condition
  re-checked in a loop (two-phase block, no lost wakeups).
- **EOF**: empty with no writers → `read` returns 0 (sticky).
- **Broken reader**: no readers → `write` returns `-1`. There is no `SIGPIPE`;
  the writer fails, it is not killed.
- Partial writes are allowed (`min(count, free)`).
- `lseek` on a pipe fails.
- `dup` aliases the same endpoint description; the side-alive flags (not fd
  counts) decide liveness, so two read dups are still one reader.

### Locking

Lock order is `vfs_lock → pipe wq->lock`, never reversed. TTY and pipe I/O hold
a temporary description reference across the lock drop so a concurrent close
cannot free the object mid-I/O.

## Inheritance

- `vfs_inherit_stdio` — used by `sys_spawn_redir` (77) to install caller fds
  for the child's 0/1/2; `-1` means a fresh TTY.
- `vfs_fork_inherit` — used by `fork` (78) to copy the whole table at the same
  numbers onto the same open descriptions. The child table must be empty or
  fork fails.

Spawn inherits nothing by default (fresh stdio); file fds stay with the opener.
Whole-table sharing is fork's job.

## Vnode Interface

`include/vnode.h` defines the filesystem-facing operations:

```c
typedef enum { V_REG, V_DIR, V_DEV } vnode_type_t;

struct vnode {
    vnode_type_t    type;
    uint32_t        refcount;
    uint64_t        size;
    uint32_t        inode_num;
    struct vnode_ops *ops;
    void            *fs_data;
};
```

`struct vnode_ops`:

```text
open, read, write, lookup, create, truncate,
mkdir, unlink, readdir, sync, release
```

All operations return 0 or a negative POSIX error code. `kzfs_v4_vnode_ops`
implements the full interface. Reads and writes are chunked at 1 MiB.
Directories reject read/write/lseek. `unlink` refuses non-empty directories
(`-EEXIST`).

## Related Documentation

- [Filesystem Overview](README.md)
- [KyuzenFS Format](format.md)
- [Process Model](../kernel/processes.md) — fd lifecycle across fork/spawn/exit
- [Shell & CLI](../userspace/shell.md) — redirection and pipelines use this layer
