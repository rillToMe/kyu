// Host-side unit test: P0 Phase 5 pipes + shell redirection/pipelines.
//
// Two layers (no scheduler/SMP/KyuzenFS on host):
//
// A. Kernel pipe LOGIC mock (mirrors kernel/vfs_fd.c decision table):
//    circular buffer, side-alive counts, EOF, would-block, dup/close
//    accounting, both-or-neither rollback, leak accounting.
//
// B. The REAL apps/shell_core.c, compiled on host against stub syscalls
//    backed by a mock fd table: pins the actual operator scan, stage
//    split, redir position rules, spawn_redir specs, parent close
//    discipline, and waitpid reaping — not a reimplementation.
//
// True SMP blocking/wakeup is covered by the QEMU pipe_test app (two
// tasks, 4 CPUs), not here: tasks/lapic cannot run on host.
//
// Build: clang -Iinclude test/pipe_test.c apps/shell_core.c -o test/pipe_test
// Run:   ./test/pipe_test  (or: make test-pipe)

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
// NOTE: no <string.h> — include/string.h (kernel freestanding) shadows the
// host header under -Iinclude. Tiny local helpers instead.
static void t_copy(char* d, const char* s, int cap) {
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}
static int t_contains(const char* h, const char* n) {
    if (!*n) return 1;
    for (int i = 0; h[i]; i++) {
        int k = 0;
        while (n[k] && h[i + k] == n[k]) k++;
        if (!n[k]) return 1;
    }
    return 0;
}

#include "proc.h"    // spawn_stdio_t, spawn_stdio_valid, SYS_SPAWN_REDIR
#include "cred.h"
#include "task.h"
#include "vfs.h"     // VFS_MAX_FDS, VFS_O_*, SYS_PIPE, VFS_PIPE_CAP
#include "shell.h"   // real shell engine under test
#include "userlib.h" // stubbed syscalls below

// ================= A. kernel pipe logic mock =================
#define PCAP VFS_PIPE_CAP
#define WOULDBLOCK (-3)

typedef struct {
    unsigned char buf[PCAP];
    uint32_t rpos, wpos, used;
    uint32_t readers, writers;   // side-alive flags (dup does NOT bump these)
    int freed;
} mpipe_t;

typedef struct {
    int used, kind;   // 0=file, 1=pipeR, 2=pipeW
    int refs;         // description refcount (dup aliases, refs++)
    mpipe_t* pipe;
    int freed;
} mdesc_t;

static mpipe_t mpool[16];
static int mnpipe;
static mdesc_t mdesc[32];
static int mndesc;
static int m_frees;   // endpoint descriptions freed
static int m_pipefrees;

static mpipe_t* mpipe_new(void) {
    assert(mnpipe < 16);
    mpipe_t* p = &mpool[mnpipe++];
    p->rpos = p->wpos = p->used = 0;
    p->readers = p->writers = 1;
    p->freed = 0;
    return p;
}

static mdesc_t* mdesc_new(int kind, mpipe_t* p) {
    assert(mndesc < 32);
    mdesc_t* d = &mdesc[mndesc++];
    d->used = 1; d->kind = kind; d->refs = 1; d->pipe = p; d->freed = 0;
    return d;
}

static void mdesc_put(mdesc_t* d) {
    assert(d && !d->freed && d->refs > 0);   // never underflow / double-release
    if (--d->refs > 0) return;
    d->freed = 1; m_frees++;
    if (d->kind == 1 || d->kind == 2) {
        mpipe_t* p = d->pipe;
        if (d->kind == 1) p->readers = 0; else p->writers = 0;
        if (p->readers == 0 && p->writers == 0 && !p->freed) {
            p->freed = 1; m_pipefrees++;
        }
    }
}

// Returns bytes, 0=EOF, WOULDBLOCK, -1=broken/closed.
static int mread(mdesc_t* d, unsigned char* out, uint32_t count) {
    if (d->kind != 1) return -1;
    mpipe_t* p = d->pipe;
    if (p->used > 0) {
        uint32_t n = (count < p->used) ? count : p->used;
        for (uint32_t i = 0; i < n; i++) {
            out[i] = p->buf[p->rpos];
            p->rpos = (p->rpos + 1) % PCAP;
        }
        p->used -= n;
        return (int)n;
    }
    if (p->writers == 0) return 0;   // EOF
    return WOULDBLOCK;               // empty + writers alive -> block
}

static int mwrite(mdesc_t* d, const unsigned char* in, uint32_t count) {
    if (d->kind != 2) return -1;
    mpipe_t* p = d->pipe;
    if (p->readers == 0) return -1;  // broken: no SIGPIPE, fail -1
    if (p->used >= PCAP) return WOULDBLOCK;   // full + readers alive -> block
    uint32_t free = PCAP - p->used;
    uint32_t n = (count < free) ? count : free;   // partial writes allowed
    for (uint32_t i = 0; i < n; i++) {
        p->buf[p->wpos] = in[i];
        p->wpos = (p->wpos + 1) % PCAP;
    }
    p->used += n;
    return (int)n;
}

// ================= B. stub syscalls + mock fd table for shell =================
// (Unnamed-namespace pollution avoided: plain static C names are fine here;
// the REAL shell symbols come from shell_core.c, syscalls are all stubbed.)

#define MOCK_FDS 16
static int s_used[MOCK_FDS];
static int s_ispipe[MOCK_FDS];
static int s_flags[MOCK_FDS];
static char s_path[MOCK_FDS][40];
static uint32_t s_openflags[MOCK_FDS];

typedef struct {
    char app[40]; int argc; int32_t fds[3]; int pid; int reaped;
    int alive;    // 1 while present in proc_list
    int zombie;   // 1 exited-unreaped (P0.3: kill targets alive only)
    int code;     // exit code once zombie
    int endless;  // never exits on its own (mock long-runner like kill_test child)
} spawnrec_t;
static spawnrec_t spawns[16];
static int nspawns, nextpid = 100;

// sys_kill mock: records every attempt; alive -> zombie(125); zombie/
// unknown/denied -> -1 (P0.3: no double-clean, auth enforced).
static int kill_recs[32];
static int nkills;
static int deny_pid = -1;
static int stray_hint;   // set when a stray (non-child) pid is offered

static char cap_out[4096];
static int cap_n;

static void capio(void* ctx, const char* t) {
    (void)ctx;
    while (*t && cap_n < (int)sizeof(cap_out) - 1) cap_out[cap_n++] = *t++;
    cap_out[cap_n] = '\0';
}

static int s_alloc(void) {
    for (int i = 0; i < MOCK_FDS; i++)
        if (!s_used[i]) return i;
    return -1;
}

static int known_app(const char* path) {
    return t_contains(path, "echo.elf") || t_contains(path, "cat.elf") ||
           t_contains(path, "pipe_test.elf") || t_contains(path, "fd_test.elf") ||
           t_contains(path, "spin.elf");
}

// --- stubs required to link shell_core.c ---
int sys_open(const char* path, uint32_t flags) {
    int fd = s_alloc();
    if (fd < 0) return -1;
    if ((flags & VFS_O_CREAT) == 0 && t_contains(path, "nope")) return -1;
    s_used[fd] = 1; s_ispipe[fd] = 0; s_flags[fd] = (int)flags;
    s_openflags[fd] = flags;
    t_copy(s_path[fd], path, sizeof(s_path[fd]));
    return fd;
}
int sys_read_fd(int fd, void* buf, uint32_t count) { (void)fd; (void)buf; (void)count; return -1; }
int sys_write_fd(int fd, const void* buf, uint32_t count) { (void)fd; (void)buf; (void)count; return -1; }
int sys_lseek(int fd, int32_t off, int w) { (void)fd; (void)off; (void)w; return -1; }
int sys_close(int fd) {
    if (fd < 0 || fd >= MOCK_FDS || !s_used[fd]) return -1;
    s_used[fd] = 0;
    return 0;
}
int sys_dup(int o) { (void)o; return -1; }
int sys_dup2(int o, int n) { (void)o; (void)n; return -1; }
int sys_pipe(int fds[2]) {
    int a = s_alloc();
    if (a < 0) return -1;
    s_used[a] = 1;
    int b = s_alloc();
    if (b < 0) { s_used[a] = 0; return -1; }   // both-or-neither rollback
    s_used[b] = 1;
    s_ispipe[a] = 1; s_ispipe[b] = 1;
    fds[0] = a; fds[1] = b;
    return 0;
}
static int32_t mock_self = 7;   // shell's own pid (0 = console: auto-reap)
int sys_spawn_redir(char* fn, int argc, char** argv, spawn_stdio_t* spec) {
    (void)argv;
    if (!known_app(fn)) return -1;
    if (argc < 1 || argc > PROC_MAX_ARGC) return -1;
    assert(nspawns < 16);
    // Validate inherited fds exist (kernel would resolve+share them).
    for (int i = 0; i < 3; i++) {
        int32_t f = spec ? ((int32_t*)spec)[i] : -1;
        if (f >= 0 && (f >= MOCK_FDS || !s_used[f])) return -1;
    }
    spawnrec_t* r = &spawns[nspawns++];
    t_copy(r->app, fn, sizeof(r->app));
    r->argc = argc;
    r->endless = t_contains(fn, "spin") ? 1 : 0;
    // Task-0 children auto-reap synchronously: born already gone.
    r->alive = (mock_self == 0) ? 0 : 1;
    r->zombie = (r->alive && !r->endless) ? 1 : 0;
    r->code = 0;
    r->reaped = 0;
    for (int i = 0; i < 3; i++) r->fds[i] = spec ? ((int32_t*)spec)[i] : -1;
    r->pid = nextpid++;
    r->reaped = 0;
    return r->pid;
}
int sys_waitpid(int pid, int* status, int options) {
    (void)options;
    for (int i = 0; i < nspawns; i++) {
        if (spawns[i].pid == pid && !spawns[i].reaped) {
            if (!spawns[i].zombie) return -1;   // still running: would block
            spawns[i].reaped = 1;
            spawns[i].alive = 0;
            if (status) *status = spawns[i].code;
            return pid;
        }
    }
    return -1;
}
int sys_spawn(char* f) { (void)f; return -1; }
int sys_spawn_argv(char* f, int a, char** v) { (void)f; (void)a; (void)v; return -1; }
// P0 Phase 6C: fork mock. Mode 0 (default): kernel-context failure (-1) so
// the shell takes the legacy spawn_redir path (all B-group behavior).
// Mode 1: instant child that already exited (like born-zombie spawns);
// the shell proceeds as parent with a forked pid. The fork-CHILD path
// (dup2/execve) never runs on host — covered by QEMU fork_test T8.
static int mock_fork_mode;
static int mock_fork_endless;   // mode-1 forks born RUNNING (killable)
static int fork_calls, execve_calls;
int sys_fork(void) {
    fork_calls++;
    if (!mock_fork_mode) return -1;
    assert(nspawns < 16);
    spawnrec_t* r = &spawns[nspawns++];
    t_copy(r->app, "forked-stage", sizeof(r->app));
    r->argc = 1;
    r->fds[0] = r->fds[1] = r->fds[2] = -2;   // via fork, not spawn_redir
    r->endless = 0; r->alive = (mock_self == 0) ? 0 : 1;
    r->zombie = (r->alive && !r->endless) ? 1 : 0; r->code = 0; r->reaped = 0;
    if (mock_fork_endless && mock_fork_mode) { r->endless = 1; r->zombie = 0; }
    r->pid = nextpid++;
    return r->pid;
}
int sys_execve(char* path, int argc, char** argv) {
    (void)path; (void)argc; (void)argv;
    execve_calls++;
    return -1;   // parent flows never exec; child path unreachable on host
}
void sys_exit_code(int code) __attribute__((noreturn));
void sys_exit_code(int code) {
    // Reached only if the shell wrongly entered the fork-child path on
    // host: fail loudly instead of continuing as a phantom second shell.
    printf("[pipe] FATAL: sys_exit_code(%d) on host — child path leaked\n", code);
    assert(0);
    for (;;) { }
}
int sys_file_exists(char* f) { return known_app(f) ? 1 : 0; }
int sys_read_file_to_buffer(char* a, char* b, uint32_t c) { (void)a; (void)b; (void)c; return 0; }
int sys_create_file(char* a, char* b, uint32_t c) { (void)a; (void)b; (void)c; return 0; }
uint32_t sys_file_size(char* f) { (void)f; return 0; }
void fs_delete(char* f) { (void)f; }
int fs_format(void) { return -1; }
int sys_mkdir(char* p) { (void)p; return 0; }
int sys_get_file_list(char* p, file_info_t* b, int m) { (void)p; (void)b; (void)m; return 0; }
uint32_t sys_get_uid(void) { return 0; }
int sys_set_uid(uint32_t u) { (void)u; return 0; }
int current_username(char* o, uint32_t c) { if (c) o[0] = '\0'; return 0; }
int current_password_match(const char* p) { (void)p; return 0; }
int sys_ping(const char* h) { (void)h; return -1; }
void sys_shutdown(void) {}
void sys_reboot(void) {}
uint64_t sys_uptime(void) { return 0; }
uint64_t sys_total_ram(void) { return 0; }
uint64_t sys_used_ram(void) { return 0; }
uint32_t sys_get_total_disk(void) { return 0; }
uint32_t sys_get_used_disk(void) { return 0; }
uint32_t sys_get_cpu_usage(void) { return 0; }
void get_cpu_string(char* b) { if (b) b[0] = '\0'; }
void sys_get_time(uint32_t* t) { for (int i = 0; i < 6; i++) t[i] = 0; }
void sys_sleep(uint32_t m) { (void)m; }
int32_t sys_getpid(void) { return mock_self; }
int sys_proc_list(proc_info_t* b, int m) {
    // Task-0 children auto-reap synchronously (no zombie): invisible here.
    // Other tasks' alive+zombie spawns linger like real snapshots.
    if (mock_self == 0) return 0;
    int n = 0;
    for (int i = 0; i < nspawns && n < m; i++) {
        if (!spawns[i].alive || spawns[i].reaped) continue;
        b[n].pid = spawns[i].pid; b[n].ppid = mock_self;
        b[n].state = spawns[i].zombie ? 5 : 1;
        b[n].exit_code = spawns[i].code;
        n++;
    }
    return n;
}
int sys_kill(int p) {
    if (nkills < 32) kill_recs[nkills++] = p;
    if (p == deny_pid) return -1;   // authorization denied: rejected, untouched
    for (int i = 0; i < nspawns; i++) {
        if (spawns[i].pid == p) {
            if (!spawns[i].alive || spawns[i].zombie) return -1;   // exited: no double-clean
            spawns[i].zombie = 1;
            spawns[i].code = 125;   // PROC_KILL_EXIT_CODE
            return 0;
        }
    }
    stray_hint = 1;   // test fails if this ever trips (see C6)
    return -1;
}
int sys_socket(void) { return -1; }
int sys_connect(int s, uint32_t i, uint16_t p) { (void)s; (void)i; (void)p; return -1; }
int sys_send(int s, const void* b, uint32_t l) { (void)s; (void)b; (void)l; return -1; }
int sys_recv(int s, void* b, uint32_t l) { (void)s; (void)b; (void)l; return -1; }
int sys_sock_close(int s) { (void)s; return -1; }

// Scripted Ctrl-C source for the shell join: values consumed in order,
// 1 = Ctrl-C arrived, 0 = nothing; exhausted script reads 0.
static int poll_script[8];
static int poll_len, poll_idx;
static int mock_natural_exit_on_poll;   // C4: endless exits win the race

static void shell_reset(void) {
    for (int i = 0; i < MOCK_FDS; i++) { s_used[i] = 0; s_openflags[i] = 0; }
    nspawns = 0; nextpid = 100;
    nkills = 0; deny_pid = -1; stray_hint = 0;
    poll_len = 0; poll_idx = 0; mock_natural_exit_on_poll = 0;
    mock_fork_mode = 0; fork_calls = 0; execve_calls = 0;
    mock_fork_endless = 0;
    cap_n = 0; cap_out[0] = '\0';
}

// Flip endless mock-procs to zombie-0 ("exited naturally just now").
static void mock_natural_exit(void) {
    for (int i = 0; i < nspawns; i++) {
        if (spawns[i].alive && !spawns[i].zombie) {
            spawns[i].zombie = 1;
            spawns[i].code = 0;
        }
    }
}

// Scripted Ctrl-C source for the shell join: values consumed in order,
// 1 = Ctrl-C arrived, 0 = nothing; exhausted script reads 0.
static int mock_poll(void* ctx) {
    (void)ctx;
    if (poll_idx < poll_len) {
        int v = poll_script[poll_idx++];
        if (v && mock_natural_exit_on_poll) mock_natural_exit();
        return v;
    }
    return 0;
}

// The single file open performed by the last shell line: its recorded flags.
static uint32_t last_open_flags(void) {
    for (int i = MOCK_FDS - 1; i >= 0; i--) {
        if (s_openflags[i]) return s_openflags[i];
    }
    return 0;
}

int main(void) {
    // ===== A1: pipe allocates two fds; direction enforced =====
    {
        mpipe_t* p = mpipe_new();
        mdesc_t* r = mdesc_new(1, p);
        mdesc_t* w = mdesc_new(2, p);
        unsigned char b[8];
        assert(mwrite(r, (unsigned char*)"x", 1) == -1);  // read end rejects write
        assert(mread(w, b, 1) == -1);                     // write end rejects read
        mdesc_put(r); mdesc_put(w);
        assert(p->freed);
    }
    // ===== A2: write/read preserves bytes; partial read =====
    {
        mpipe_t* p = mpipe_new();
        mdesc_t* r = mdesc_new(1, p);
        mdesc_t* w = mdesc_new(2, p);
        unsigned char b[8];
        assert(mwrite(w, (unsigned char*)"ABCDEFGH", 8) == 8);
        assert(mread(r, b, 3) == 3 && b[0] == 'A' && b[2] == 'C');
        assert(mread(r, b, 5) == 5 && b[0] == 'D' && b[4] == 'H');
        mdesc_put(r); mdesc_put(w);
    }
    // ===== A3: empty + writers -> would block; close writer -> EOF =====
    {
        mpipe_t* p = mpipe_new();
        mdesc_t* r = mdesc_new(1, p);
        mdesc_t* w = mdesc_new(2, p);
        unsigned char b[8];
        assert(mread(r, b, 8) == WOULDBLOCK);
        assert(mwrite(w, (unsigned char*)"EO", 2) == 2);
        mdesc_put(w);   // last writer closes
        assert(p->writers == 0);
        assert(mread(r, b, 2) == 2 && b[0] == 'E');
        assert(mread(r, b, 2) == 0);   // EOF, sticky
        assert(mread(r, b, 2) == 0);
        mdesc_put(r);
        assert(p->freed);   // freed only after final ref
    }
    // ===== A4: full + readers -> would block; drain wakes; broken fails =====
    {
        mpipe_t* p = mpipe_new();
        mdesc_t* r = mdesc_new(1, p);
        mdesc_t* w = mdesc_new(2, p);
        unsigned char fill[PCAP], drain[PCAP];
        for (uint32_t i = 0; i < PCAP; i++) fill[i] = (unsigned char)(i & 0xFF);
        assert(mwrite(w, fill, PCAP) == (int)PCAP);
        assert(mwrite(w, fill, 1) == WOULDBLOCK);
        assert(mread(r, drain, PCAP) == (int)PCAP);
        for (uint32_t i = 0; i < PCAP; i++) assert(drain[i] == (unsigned char)(i & 0xFF));
        mdesc_put(r);   // last reader closes
        assert(p->readers == 0);
        assert(mwrite(w, fill, 1) == -1);   // broken: fail, no SIGPIPE
        mdesc_put(w);
        assert(p->freed);
    }
    // ===== A5: dup shares endpoint; side flags vs desc refcount =====
    {
        mpipe_t* p = mpipe_new();
        mdesc_t* r = mdesc_new(1, p);
        mdesc_t* w = mdesc_new(2, p);
        r->refs++;   // dup(read): same description, second reference
        assert(r->refs == 2 && p->readers == 1);   // NOT two underlying endpoints
        mdesc_put(r);   // close one duplicate: side still alive
        assert(!r->freed && p->readers == 1);
        unsigned char b[4];
        assert(mwrite(w, (unsigned char*)"Z", 1) == 1);
        assert(mread(r, b, 1) == 1 && b[0] == 'Z');
        mdesc_put(r);   // final close: side drops
        assert(r->freed && p->readers == 0);
        mdesc_put(w);
        assert(p->freed);
    }
    // ===== A6: wraparound integrity (64KB patterned, positions cycle) =====
    {
        mpipe_t* p = mpipe_new();
        mdesc_t* r = mdesc_new(1, p);
        mdesc_t* w = mdesc_new(2, p);
        unsigned char chunk[512], back[512];
        uint32_t expect = 0;
        for (uint32_t blk = 0; blk < 128; blk++) {   // 128*512 = 64KB
            for (int i = 0; i < 512; i++) chunk[i] = (unsigned char)((expect + i) & 0xFF);
            assert(mwrite(w, chunk, 512) == 512);
            assert(mread(r, back, 512) == 512);
            for (int i = 0; i < 512; i++) assert(back[i] == (unsigned char)((expect + i) & 0xFF));
            expect += 512;
        }
        assert(expect == 65536u);
        mdesc_put(r); mdesc_put(w);
        assert(p->freed);
    }
    // ===== A7: invalid + spawn_stdio_valid + syscall numbers =====
    {
        assert(spawn_stdio_valid(-1) && spawn_stdio_valid(0) && spawn_stdio_valid(15));
        assert(!spawn_stdio_valid(-2) && !spawn_stdio_valid(16));
        assert(SYS_PIPE == 76 && SYS_SPAWN_REDIR == 77);
        assert(VFS_PIPE_CAP == 4096);
    }
    printf("[pipe] kernel-logic mock: 7 groups passed\n");

    // ===== B: real shell engine =====
    shell_io_t io;
    io.out = capio; io.err = capio; io.clear = NULL; io.ctx = NULL;
    io.poll_input = mock_poll;
    shell_t* sh = shell_init(&io);

    // B1: echo hello | cat — one pipe, specs, close discipline, reaps
    shell_reset();
    assert(shell_execute(sh, "echo hello | cat") == SHELL_OK);
    assert(nspawns == 2);
    assert(t_contains(spawns[0].app, "echo.elf") && spawns[0].fds[0] == -1);
    assert(t_contains(spawns[1].app, "cat.elf") && spawns[1].fds[1] == -1);
    assert(spawns[0].fds[1] >= 0 && spawns[1].fds[0] >= 0);   // shared pipe ends
    assert(spawns[0].fds[1] != spawns[1].fds[0]);
    {
        // both pipe ends closed in parent after spawn (EOF discipline)
        int pr = spawns[1].fds[0], pw = spawns[0].fds[1];
        assert(s_used[pr] == 0 && s_used[pw] == 0);
        assert(spawns[0].reaped && spawns[1].reaped);
    }

    // B2: echo hi > out.txt — TRUNC, no APPEND
    shell_reset();
    assert(shell_execute(sh, "echo hi > out.txt") == SHELL_OK);
    assert(nspawns == 1 && spawns[0].fds[1] >= 0 && spawns[0].fds[0] == -1);
    {
        uint32_t fl = last_open_flags();
        assert((fl & VFS_O_TRUNC) && !(fl & VFS_O_APPEND) && (fl & VFS_O_CREAT));
    }
    // B3: cmd >> f — APPEND set, TRUNC clear
    shell_reset();
    assert(shell_execute(sh, "echo hi >> out.txt") == SHELL_OK);
    {
        uint32_t fl = last_open_flags();
        assert((fl & VFS_O_APPEND) && !(fl & VFS_O_TRUNC) && (fl & VFS_O_CREAT));
        assert(nspawns == 1);
    }

    // B4: cat < in.txt — RDONLY open, inherited as stdin
    shell_reset();
    assert(shell_execute(sh, "cat < in.txt") == SHELL_OK);
    assert(nspawns == 1 && spawns[0].fds[0] >= 0 && spawns[0].fds[1] == -1);

    // B5: three stages — two pipes, middle inherits both
    shell_reset();
    assert(shell_execute(sh, "echo a | cat | cat") == SHELL_OK);
    assert(nspawns == 3);
    assert(spawns[1].fds[0] >= 0 && spawns[1].fds[1] >= 0);
    assert(spawns[1].fds[0] != spawns[1].fds[1]);
    for (int i = 0; i < MOCK_FDS; i++) assert(s_used[i] == 0);   // nothing leaked

    // B6: builtin stage rejected; missing app rejected
    shell_reset();
    assert(shell_execute(sh, "help | cat") == SHELL_ERR);
    assert(nspawns == 0);
    assert(shell_execute(sh, "nosuchapp | cat") == SHELL_ERR);
    assert(nspawns == 0);
    assert(shell_execute(sh, "echo hi | start foo") == SHELL_ERR);

    // B7: syntax errors — no spawn, no leak
    shell_reset();
    assert(shell_execute(sh, "echo hi |") == SHELL_ERR);
    assert(shell_execute(sh, "| cat") == SHELL_ERR);
    assert(shell_execute(sh, "echo hi >") == SHELL_ERR);
    assert(shell_execute(sh, "> out.txt") == SHELL_ERR);
    assert(shell_execute(sh, "cat < a < b") == SHELL_ERR);
    assert(shell_execute(sh, "echo hi > a > b") == SHELL_ERR);
    assert(shell_execute(sh, "cat < in > out | cat") == SHELL_ERR);  // '>' must be last stage
    assert(nspawns == 0);
    for (int i = 0; i < MOCK_FDS; i++) assert(s_used[i] == 0);

    // B8: "a>b" without spaces is one word -> normal dispatch, no pipe made
    shell_reset();
    assert(shell_execute(sh, "a>b") == SHELL_ERR);
    assert(nspawns == 0);
    assert(t_contains(cap_out, "Perintah tidak dikenali"));

    // B9: plain commands unaffected (no pipe fds, builtin path)
    shell_reset();
    assert(shell_execute(sh, "echo hello") == SHELL_OK);
    assert(nspawns == 0);
    assert(t_contains(cap_out, "hello"));

    // B10: task-0 shell (console): children auto-reap, so no waitpid —
    // spawn + close discipline still exact, statuses unreported, still OK.
    shell_reset();
    mock_self = 0;
    assert(shell_execute(sh, "echo hello | cat") == SHELL_OK);
    assert(nspawns == 2);
    assert(!spawns[0].reaped && !spawns[1].reaped);   // no wait attempted
    for (int i = 0; i < MOCK_FDS; i++) assert(s_used[i] == 0);
    mock_self = 7;

    // ===== C: foreground Ctrl-C (P0 Phase 6A) =====
    // C1: no Ctrl-C during join -> no kills, clean reap.
    shell_reset();
    poll_len = 0;   // script exhausted: silence
    assert(shell_execute(sh, "echo hi | cat") == SHELL_OK);
    assert(nkills == 0);
    assert(spawns[0].reaped && spawns[1].reaped);

    // C2: one Ctrl-C kills both stages exactly once; statuses 125 -> ERR.
    shell_reset();
    poll_script[0] = 0; poll_script[1] = 1; poll_len = 2;
    assert(shell_execute(sh, "spin x | cat") == SHELL_ERR);
    assert(nkills == 2);   // spin + cat, one attempt each
    assert(t_contains(cap_out, "^C"));
    assert(!stray_hint);
    for (int i = 0; i < nkills; i++) {
        int k = kill_recs[i];
        assert(k == spawns[0].pid || k == spawns[1].pid);   // fg only
    }

    // C3: duplicate Ctrl-C still kills each pid once (3 stages).
    shell_reset();
    poll_script[0] = 1; poll_script[1] = 1; poll_len = 2;
    assert(shell_execute(sh, "spin a | cat | cat") == SHELL_ERR);
    assert(nkills == 3);
    assert(!stray_hint);

    // C4: natural-exit race — child exits 0 just as Ctrl-C arrives:
    // kill rejected (-1, already zombie), no double-clean, still OK.
    shell_reset();
    poll_script[0] = 1; poll_len = 1;
    mock_natural_exit_on_poll = 1;
    assert(shell_execute(sh, "spin x | cat") == SHELL_OK);
    mock_natural_exit_on_poll = 0;
    assert(nkills == 2);   // attempts made, rejected gracefully
    assert(!stray_hint);

    // C5: denied kill is handled gracefully (join times out, ERR, no hang).
    shell_reset();
    deny_pid = nextpid;   // first spawn's pid: spin stage
    poll_script[0] = 1; poll_len = 1;
    assert(shell_execute(sh, "spin x | cat") == SHELL_ERR);
    deny_pid = -1;
    assert(!stray_hint);

    // C6+C10: shell reusable after interrupt; kills target current pids.
    // (Pids repeat across runs like real slot reuse; correctness = the
    // shell only ever names pids from its own current foreground list.)
    shell_reset();
    poll_script[0] = 1; poll_len = 1;
    assert(shell_execute(sh, "spin x | cat") == SHELL_ERR);
    shell_reset();
    poll_script[0] = 1; poll_len = 1;
    assert(shell_execute(sh, "spin y | cat") == SHELL_ERR);
    assert(nkills == 2);
    for (int i = 0; i < nkills; i++) {
        int k = kill_recs[i];
        assert(k == spawns[0].pid || k == spawns[1].pid);
    }
    assert(!stray_hint);
    // Plain command after interrupt works (shell state usable).
    shell_reset();
    assert(shell_execute(sh, "echo hello") == SHELL_OK);
    assert(nspawns == 0 && nkills == 0);

    // C9: poll helper is NULL-safe.
    assert(shell_poll_ctrlc(NULL) == 0);

    printf("[pipe] ctrlc: 10 groups passed\n");

    // ===== D: fork-first shell execution (P0 Phase 6C) =====
    // D1: fork attempted before spawn_redir; pids come from fork.
    shell_reset();
    mock_fork_mode = 1;
    assert(shell_execute(sh, "echo hi | cat") == SHELL_OK);
    assert(fork_calls == 2);          // one fork per stage, tried first
    assert(nspawns == 2);             // ...and both taken from fork
    assert(execve_calls == 0);        // parent never execs
    for (int i = 0; i < nspawns; i++) assert(spawns[i].fds[0] == -2);  // no redir spec
    assert(spawns[0].reaped && spawns[1].reaped);

    // D2: fallback — fork fails (kernel context) -> spawn_redir, same wiring.
    shell_reset();
    mock_fork_mode = 0;
    assert(shell_execute(sh, "echo hi | cat") == SHELL_OK);
    assert(fork_calls == 2);          // tried first every stage...
    assert(nspawns == 2);             // ...then fell back
    assert(spawns[0].fds[1] >= 0 && spawns[1].fds[0] >= 0);  // redir specs used
    assert(execve_calls == 0);

    // D3: redir + fork path keeps file flags and close discipline.
    shell_reset();
    mock_fork_mode = 1;
    assert(shell_execute(sh, "echo hi > out.txt") == SHELL_OK);
    assert(fork_calls == 1 && nspawns == 1 && execve_calls == 0);
    {
        uint32_t fl = last_open_flags();
        assert((fl & VFS_O_TRUNC) && (fl & VFS_O_CREAT));
    }
    for (int i = 0; i < MOCK_FDS; i++) assert(s_used[i] == 0);

    // D4: Ctrl-C works on forked pids too (same join machinery).
    shell_reset();
    mock_fork_mode = 1;
    mock_fork_endless = 1;
    poll_script[0] = 1; poll_len = 1;
    assert(shell_execute(sh, "spin x | spin") == SHELL_ERR);
    assert(nkills == 2);
    for (int i = 0; i < nkills; i++) {
        int k = kill_recs[i];
        assert(k == spawns[0].pid || k == spawns[1].pid);
    }
    assert(!stray_hint);
    mock_fork_endless = 0;

    printf("[pipe] forkexec: 4 groups passed\n");

    printf("[pipe] shell-engine: 10 groups passed\n");
    printf("[pipe] all pipe/host tests passed\n");
    return 0;
}
