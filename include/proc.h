#ifndef PROC_H
#define PROC_H

#include <stdint.h>

// ============================================================
// P0 Phase 2 — Process identity, argv, stdio, exit/wait.
//
// Kernel is source of truth. Userspace APIs are interfaces to it.
// No second PID system, no shell-only argv, no fake stdio table.
//
// PID = task slot id (task_t.id), stable while alive INCLUDING
// zombie. Reused only after parent reaps (ZOMBIE -> DEAD).
// Cookie (sys_get_pid/45) stays a diagnostic AS id, NOT the PID.
// ============================================================

// No-parent sentinel for kernel/system tasks. Never a valid PID.
#define PROC_NO_PARENT ((int32_t)-1)

// --- argv bounds (fit shell SHELL_LINE_MAX=256 + user stack 256KB) ---
#define PROC_MAX_ARGC    16    // argv[0..15], argv[argc]==NULL
#define PROC_MAX_ARG_LEN 64    // bytes per arg INCLUDING NUL
#define PROC_ARG_TOTAL_MAX 512 // sum of (len+1) over all args

// --- stdio (per-task, VFS-backed; see vfs.h) ---
#define PROC_STDIN  0
#define PROC_STDOUT 1
#define PROC_STDERR 2

// --- waitpid ---
#define PROC_WAIT_ANY ((int32_t)-1)  // wait for any child

// --- kill (P0 Phase 3) ---
// Deterministic KyuzenOS convention, NOT POSIX signal encoding. There are
// no WIFEXITED-style macros: a killed child reports exit_code ==
// PROC_KILL_EXIT_CODE and exit_reason == PROC_EXIT_KILLED. Normal exits
// keep their plain code with reason PROC_EXIT_NORMAL. Compare codes with
// == only; never decode them as 128+signo.
#define PROC_KILL_EXIT_CODE 125
#define PROC_EXIT_NORMAL    0
#define PROC_EXIT_KILLED    1

// --- syscalls (see kernel/syscall/; 73+ were free; 74/75 now taken
// by SYS_DUP/SYS_DUP2 in vfs.h) ---
#define SYS_EXIT_CODE   34   // extended: RBX = exit code (old void wrapper now passes 0)
#define SYS_SPAWN_ARGV  68   // RBX=path, RCX=argc, RDX=argv (user char**)
#define SYS_WAITPID     69   // RBX=pid, RCX=status* (user int* or 0), RDX=options (must 0)
#define SYS_GETPID      70   // -> task id (-1 if idle)
#define SYS_GETPPID     71   // -> parent id (PROC_NO_PARENT if none)
#define SYS_PROC_LIST   72   // RBX=buf (user proc_info_t*), RCX=max -> count/-1
#define SYS_KILL        73   // RBX=pid -> 0 / -1 (P0 Phase 3: parent/root only)
#define SYS_SPAWN_REDIR 77   // RBX=path, RCX=argc, RDX=argv, RSI=spec* (P0 Phase 5)
#define SYS_FORK        78   // -> child pid (parent), 0 (child), -1 (fail)
#define SYS_EXECVE      79   // RBX=path, RCX=argc, RDX=argv -> 0 (never returns) / -1

// Per-address-space cookie generator (P0 Phase 2, defined in
// kernel/syscall/sys_proc.c): every new AS — spawn or fork — takes the next id.
uint32_t as_cookie_next(void);

// P0 Phase 5 — explicit stdio inheritance for spawn_redir. Names the
// CALLER's fds to install as the child's 0/1/2 (-1 each = fresh console
// TTY). Only 0/1/2 are inheritable: no whole-table copy, no arbitrary
// cross-task fd surgery. Used by shell redirection/pipelines.
typedef struct {
    int32_t fd0;   // child stdin  (parent fd, or -1)
    int32_t fd1;   // child stdout (parent fd, or -1)
    int32_t fd2;   // child stderr (parent fd, or -1)
} spawn_stdio_t;

// 1 = inheritance request well-formed (-1 or a valid fd number each).
static inline int spawn_stdio_valid(int32_t fd) {
    return fd == -1 || (fd >= 0 && fd < 16);
}

// Observable process state for task manager / proc_list.
// state mirrors TASK_* (task.h); ZOMBIE keeps pid+exit_code until reaped.
typedef struct {
    int32_t  pid;
    int32_t  ppid;
    uint8_t  state;
    uint8_t  exit_reason; // PROC_EXIT_* (valid iff state == TASK_ZOMBIE)
    uint16_t _pad1;
    uint32_t uid;
    uint32_t gid;
    int32_t  exit_code;   // valid iff state == TASK_ZOMBIE
    char     name[16];
} proc_info_t;

// Kernel-internal argv/list helpers (defined in kernel/proc/proc.c,
// consumed by kernel/syscall/sys_proc.c — usercopy boundary, bounded).
// proc_copy_in_argv tidak di sini: butuh ucopy_ctx_t (usercopy.h) dan hanya
// dipakai satu TU — deklarasinya tetap lokal di sys_proc.c.
int proc_build_argv(uint64_t* stack_top_inout, int argc,
                    char kargv[][PROC_MAX_ARG_LEN], uint64_t* argv_out);
void proc_basename(const char* path, char* out, uint32_t cap);
int proc_fill_list(proc_info_t* kbuf, int max);

// --- Pure validators (host-testable, no scheduler) ---

// 1 = argc usable for a new process, 0 = reject.
static inline int proc_argc_valid(int argc) {
    return argc >= 1 && argc <= PROC_MAX_ARGC;
}

// 1 = single arg length (INCLUDING NUL) fits, 0 = reject.
static inline int proc_arg_len_valid(uint32_t len_incl_nul) {
    return len_incl_nul >= 1 && len_incl_nul <= PROC_MAX_ARG_LEN;
}

// 1 = total bytes (sum len+1) fits, 0 = reject.
static inline int proc_arg_total_valid(uint32_t total) {
    return total >= 1 && total <= PROC_ARG_TOTAL_MAX;
}

// Minimum wait policy: parent may wait only for its own child.
// pid == PROC_WAIT_ANY means "any child of self" (caller checks existence).
// Returns 1 if the wait target is allowed, 0 if rejected.
static inline int proc_can_wait(int32_t parent, int32_t child_parent, int32_t child_pid, int32_t wait_pid) {
    (void)child_pid;
    if (parent < 0) return 0;
    if (child_parent != parent) return 0;   // not our child
    if (wait_pid == PROC_WAIT_ANY) return 1;
    return wait_pid == child_pid;
}

// PID usable while alive (slot range only; liveness checked under lock).
static inline int proc_pid_in_range(int32_t pid, int max_tasks) {
    return pid >= 0 && pid < max_tasks;
}

// Minimum kill policy: who may request termination of whom.
// is_root: caller cred (kernel-owned, never userspace-provided).
// Parenthood is parent_id-based, never UID-based.
// Returns 1 if the kill request is allowed, 0 if rejected.
// Rules: target must be alive + spawned (kind gate keeps PID 0 and all
// kernel tasks unkillable); caller may be the target itself (suicide),
// root (any spawned target), or the target's parent. Zombies are already
// exited -> not killable (second kill must fail, never double-cleanup).
static inline int proc_can_kill(int caller_is_root, int32_t caller,
                                int32_t target_parent, int32_t target,
                                int target_is_spawned, int target_alive) {
    if (!target_alive || !target_is_spawned) return 0;
    if (target < 0) return 0;
    if (target == caller) return 1;                 // suicide (spawned only)
    if (caller_is_root) return 1;                   // root: any spawned task
    if (caller < 0) return 0;
    return target_parent == caller;                 // normal user: own child
}

#endif // PROC_H
