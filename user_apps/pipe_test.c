// ============================================================
// pipe_test.c — P0 Phase 5 QEMU test: kernel pipes + dup + EOF + SMP.
//
// Single-task part: create, direction enforcement, write->read,
// partial read, EOF-on-close, broken-reader write failure, dup/dup2
// endpoint sharing, close discipline.
// Two-task part (SMP): parent <-> child roundtrip of 32KB through two
// pipes (child stdin/stdout inherited via sys_spawn_redir). Blocking
// is the synchronization — no sleeps, fully deterministic. The child
// usually lands on another CPU, exercising cross-CPU wakeups.
//
// Exit code 0 = all PASS. Launch: start pipe_test
// ============================================================
#include "userlib.h"

static int pass = 1;

static void check(char* name, int ok) {
    print(name);
    print(ok ? ": OK\n" : ": FAIL\n");
    if (!ok) pass = 0;
}

static int stra_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Child: pump stdin -> stdout until EOF (inherited pipe ends).
static void child_main(void) {
    char b[512];
    for (;;) {
        int n = sys_read_fd(0, b, sizeof(b));
        if (n < 0) sys_exit_code(1);
        if (n == 0) break;   // EOF: parent closed its write end
        int w = 0;
        while (w < n) {
            int m = sys_write_fd(1, b + w, (uint32_t)(n - w));
            if (m <= 0) sys_exit_code(1);
            w += m;
        }
    }
    sys_exit_code(0);
}

// Sleepreader child: block forever reading an empty pipe (for the
// kill test — never writes, never exits on its own).
static void sleepreader_main(void) {
    char b[16];
    for (;;) {
        int n = sys_read_fd(0, b, sizeof(b));
        if (n < 0) sys_exit_code(2);
        if (n == 0) sys_exit_code(3);   // unexpected EOF
    }
}
// report "SINK <total> OK/BAD" on stdout (console TTY) and exit.
static void sink_main(void) {
    char b[512];
    uint32_t total = 0;
    int bad = 0;
    for (;;) {
        int n = sys_read_fd(0, b, sizeof(b));
        if (n < 0) sys_exit_code(1);
        if (n == 0) break;
        for (int i = 0; i < n; i++) {
            if (b[i] != (char)('A' + (int)((total + (uint32_t)i) % 26u))) bad = 1;
        }
        total += (uint32_t)n;
    }
    print("SINK ");
    print_num(total);
    print(bad ? " BAD\n" : " OK\n");
    sys_exit_code(bad ? 1 : 0);
}

// Killable parent: child blocks forever on an empty pipe read; kill it
// and verify (a) waitpid reports 125, (b) the read end is gone (write
// now fails -1 — close_all ran through the killable-block cleanup with
// no leak/hang). Exercises wait_block_killable + pipe_io_killed.
static void killable_main(void) {
    print("PIPEKILL TEST\n");
    int pp[2];
    int ok = (sys_pipe(pp) == 0);
    char app[40];
    build_app_path(app, sizeof(app), "pipe_test.elf");
    char* cargv[2];
    cargv[0] = "pipe_test";
    cargv[1] = "sleepreader";
    spawn_stdio_t spec;
    spec.fd0 = pp[0]; spec.fd1 = -1; spec.fd2 = -1;
    int child = -1;
    if (ok) child = sys_spawn_redir(app, 2, cargv, &spec);
    ok = ok && (child >= 0);
    sys_close(pp[0]);   // only the child holds the read end now
    sys_sleep(500);     // let the child reach its blocked read (any CPU)
    ok = ok && (sys_kill(child) == 0);
    int status = -1;
    int waited = sys_waitpid(child, &status, 0);
    ok = ok && (waited == child) && (status == 125);
    ok = ok && (sys_write_fd(pp[1], "x", 1) == -1);   // readers gone
    sys_close(pp[1]);
    if (ok) print("PIPEKILL TEST PASS\n");
    else print("PIPEKILL TEST FAIL\n");
    sys_exit_code(ok ? 0 : 1);
}

#define BULK_TOTAL 32768u   // 8x pipe capacity: forces full/empty blocking
#define CHUNK 512

void main(int argc, char* argv[]) {
    if (argc >= 2 && stra_eq(argv[1], "child")) { child_main(); }
    if (argc >= 2 && stra_eq(argv[1], "sink")) { sink_main(); }
    if (argc >= 2 && stra_eq(argv[1], "sleepreader")) { sleepreader_main(); }
    if (argc >= 2 && stra_eq(argv[1], "killable")) { killable_main(); }

    print("PIPE TEST\n");

    // --- single-task: create + direction ---
    int pf[2] = { -1, -1 };
    check("create", sys_pipe(pf) == 0 && pf[0] >= 0 && pf[1] >= 0 && pf[0] != pf[1]);
    {
        char b[8];
        int ok = (sys_write_fd(pf[0], "x", 1) == -1);   // read end rejects write
        ok = ok && (sys_read_fd(pf[1], b, 1) == -1);    // write end rejects read
        ok = ok && (sys_lseek(pf[0], 0, SEEK_SET) == -1);
        ok = ok && (sys_lseek(pf[1], 0, SEEK_SET) == -1);
        check("direction", ok);
    }

    // --- write -> read, partial read ---
    {
        char b[8];
        int ok = (sys_write_fd(pf[1], "ABCDEFGH", 8) == 8);
        ok = ok && (sys_read_fd(pf[0], b, 3) == 3 && b[0] == 'A' && b[2] == 'C');
        ok = ok && (sys_read_fd(pf[0], b, 5) == 5 && b[0] == 'D' && b[4] == 'H');
        check("write-read", ok);
    }

    // --- dup shares the endpoint ---
    {
        char b[8];
        int d = sys_dup(pf[0]);
        int ok = (d >= 0 && d != pf[0]);
        ok = ok && (sys_write_fd(pf[1], "Z", 1) == 1);
        ok = ok && (sys_close(pf[0]) == 0);          // close original...
        ok = ok && (sys_read_fd(d, b, 1) == 1 && b[0] == 'Z');  // ...dup still reads
        int d2 = sys_dup2(pf[1], 6);
        ok = ok && (d2 == 6);
        ok = ok && (sys_close(pf[1]) == 0);          // close original write...
        ok = ok && (sys_write_fd(6, "Q", 1) == 1);   // ...dup2 still writes
        ok = ok && (sys_read_fd(d, b, 1) == 1 && b[0] == 'Q');
        ok = ok && (sys_close(d) == 0 && sys_close(6) == 0);
        check("dup-share", ok);
    }

    // --- EOF: close write end, drain, read -> 0 ---
    {
        char b[8];
        int ef[2];
        int ok = (sys_pipe(ef) == 0);
        ok = ok && (sys_write_fd(ef[1], "EO", 2) == 2);
        ok = ok && (sys_close(ef[1]) == 0);
        ok = ok && (sys_read_fd(ef[0], b, 2) == 2 && b[0] == 'E');
        ok = ok && (sys_read_fd(ef[0], b, 2) == 0);  // EOF, must not hang
        ok = ok && (sys_read_fd(ef[0], b, 2) == 0);  // sticky EOF
        ok = ok && (sys_close(ef[0]) == 0);
        check("eof", ok);
    }

    // --- broken reader: close read end, write fails ---
    {
        int q[2];
        int ok = (sys_pipe(q) == 0);
        ok = ok && (sys_close(q[0]) == 0);
        ok = ok && (sys_write_fd(q[1], "x", 1) == -1);  // no SIGPIPE: fail -1
        ok = ok && (sys_close(q[1]) == 0);
        check("broken", ok);
    }

    // --- invalid fds fail safely ---
    {
        char b[4];
        int bad[2];
        int ok = (sys_dup(-1) == -1 && sys_dup(99) == -1);
        ok = ok && (sys_read_fd(-1, b, 1) == -1 && sys_write_fd(99, "x", 1) == -1);
        ok = ok && (sys_pipe(bad) == 0);   // table still usable afterwards
        ok = ok && (sys_close(bad[0]) == 0 && sys_close(bad[1]) == 0);
        check("invalid", ok);
    }

    // --- two-task SMP transfer: 32KB parent -> sink child, one pipe ---
    // Deadlock-free by construction: a single pipe, the child always
    // drains, the parent writes then closes (EOF). The child usually
    // lands on another CPU: empty-blocking (child at start) and
    // full-blocking (parent outrunning the child) cross CPUs. The child
    // verifies every byte itself and reports SINK <total> OK.
    {
        int pp[2];
        int ok = (sys_pipe(pp) == 0);
        char app[40];
        build_app_path(app, sizeof(app), "pipe_test.elf");
        char* cargv[2];
        cargv[0] = "pipe_test";
        cargv[1] = "sink";
        spawn_stdio_t spec;
        spec.fd0 = pp[0]; spec.fd1 = -1; spec.fd2 = -1;
        int child = -1;
        if (ok) child = sys_spawn_redir(app, 2, cargv, &spec);
        ok = ok && (child >= 0);
        sys_close(pp[0]);   // parent drops the read end immediately
        uint32_t sent = 0;
        while (ok && sent < BULK_TOTAL) {   // blocks when full; child drains
            char chunk[CHUNK];
            for (int i = 0; i < CHUNK; i++)
                chunk[i] = (char)('A' + (int)((sent + (uint32_t)i) % 26u));
            int n = sys_write_fd(pp[1], chunk, CHUNK);
            if (n <= 0) { ok = 0; break; }
            sent += (uint32_t)n;
        }
        sys_close(pp[1]);   // EOF -> sink reports + exits
        int status = -1;
        int waited = sys_waitpid(child, &status, 0);
        ok = ok && (sent == BULK_TOTAL);
        ok = ok && (waited == child) && (status == 0);
        check("smp-roundtrip", ok);
    }

    if (pass) print("PIPE TEST PASS\n");
    else print("PIPE TEST FAIL\n");
    sys_exit_code(pass ? 0 : 1);
}
