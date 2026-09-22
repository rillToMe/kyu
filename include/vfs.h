#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include "fs.h"

// VFS fd layer — per-task file descriptors over KyuzenFS.
//
// P0 Phase 4 model: an fd ENTRY (per task) references a shared OPEN
// DESCRIPTION (refcounted heap object owning buffer + offset + flags).
// dup()/dup2() install a second entry on the SAME description — one
// offset, one buffer, one dirty flag. close() drops one reference;
// the last close flushes + frees. See kernel/vfs_fd.c.

#define VFS_MAX_FDS      16     // per task
#define VFS_MAX_PATH     23     // matches kfs filename limit (22 + null)

// open flags
#define VFS_O_RDONLY  0x0
#define VFS_O_WRONLY  0x1
#define VFS_O_RDWR    0x2
#define VFS_O_CREAT   0x4
#define VFS_O_TRUNC   0x8
#define VFS_O_APPEND  0x10

// lseek whence
#define VFS_SEEK_SET  0
#define VFS_SEEK_CUR  1
#define VFS_SEEK_END  2

// P0 Phase 5 pipes: bounded circular buffer shared by one read-end and
// one write-end open description. readers/writers are side-alive flags,
// not fd counts (dup aliases the same description).
#define VFS_PIPE_CAP     4096   // bytes per pipe (fixed; fits KyuzenOS heap)

// fd syscalls (see kernel/syscall.c; 74/75 were the first free pair
// after SYS_KILL 73 — existing numbers untouched).
#define SYS_DUP   74   // RBX=oldfd -> newfd / -1
#define SYS_DUP2  75   // RBX=oldfd, RCX=newfd -> newfd / -1
#define SYS_PIPE  76   // RBX=user int[2] -> 0 (fds[0]=read, fds[1]=write) / -1

// Fase 4 (filesystem tree) — 81-83, nomor pertama yang bebas setelah 80
// (SYS_CRASH_NOTICE). Semuanya user-safe: path/string di-copy dari user,
// tidak ada struktur internal FS yang bocor ke user-space.
#define SYS_READDIR 81  // RBX=fd, RCX=index, RDX=user name buf, RSI=cap,
                        // RDI=user uint8_t* is_dir (boleh 0) -> 0 ada / -1 habis
#define SYS_RENAME  82  // RBX=old path, RCX=new path -> 0 / -1
#define SYS_STAT    83  // RBX=path, RCX=user uint32_t* size, RDX=user
                        // uint8_t* is_dir (keduanya boleh 0) -> 0 / -1

void vfs_init(void);

// P0 Phase 2: install fd 0/1/2 (stdin/stdout/stderr -> console TTY) for a
// new task. Idempotent: existing entries are left alone. Call once per task
// after slot assignment, outside scheduler_lock (own lock), before READY.
void vfs_task_init(int task_id);

// Duplicate an fd within the CALLING task (oldfd -> lowest free fd).
// Both entries reference the SAME open description (shared offset,
// shared buffer/dirty state for files, shared device for TTY).
// Returns new fd (>=0) or -1 (invalid oldfd / table full).
int  vfs_dup(int oldfd);

// Duplicate oldfd onto exactly newfd (dup2 semantics): oldfd == newfd
// is a validated no-op; an open newfd is closed first. The new
// reference is acquired before the target is released, so sharing
// ends can never destroy the description mid-call.
// Returns newfd or -1 (invalid oldfd / newfd out of range).
int  vfs_dup2(int oldfd, int newfd);

// Return fd (>= 0) or negative on error. Each fd is valid only for the task
// that opened it.
int  vfs_open(const char* path, uint32_t flags);
int  vfs_read(int fd, void* buf, uint32_t count);
int  vfs_write(int fd, const void* buf, uint32_t count);
int  vfs_lseek(int fd, int32_t offset, int whence);
int  vfs_close(int fd);

// Enumerasi direktori lewat fd yang dibuka dengan O_RDONLY pada path
// direktori (handle VFS_KIND_DIR — read/write/lseek ditolak). index 0 = entri
// pertama, isi mentah TERMASUK "." dan "..".
// Return 0 = ada (name_out NUL-terminated oleh kernel, *type_out 1 = direktori
// / 0 = file), -1 = habis atau fd bukan direktori.
int  vfs_readdir(int fd, uint32_t index, char* name_out, uint32_t name_cap,
                 uint8_t* type_out);

// Create a pipe: fds[0] = read end (O_RDONLY), fds[1] = write end
// (O_WRONLY), both in the CALLING task on one shared pipe object.
// Both-or-neither: if two free slots are not available, nothing is
// allocated and -1 is returned. Returns 0 or -1.
int  vfs_pipe(int out[2]);

// Explicit stdio inheritance for spawn (sys_spawn_redir): replace the
// CHILD's fd 0/1/2 entries with shared references to the PARENT's open
// descriptions named by spec[0..2] (-1 = keep the child's fresh TTY).
// Child must not be runnable yet. Returns 0 or -1 (bad fd/owner);
// on failure the caller must release the child (vfs_close_all).
int  vfs_inherit_stdio(int child, int parent, const int spec[3]);

// Whole-table inheritance for fork(): every used parent fd reappears at
// the same number sharing the same open description. Child table must
// be completely free. Returns 0 or -1.
int  vfs_fork_inherit(int child, int parent);

// Release every fd owned by a task (called on task_exit to avoid leaks).
void vfs_close_all(int task_id);

#endif // VFS_H
