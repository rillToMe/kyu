// ============================================================
// fork_test.c — P0 Phase 6B QEMU test: fork() return values, PID/PPID,
// memory independence, FD sharing, pipe inheritance, multi-fork, kill.
//
// Parent-driven: each fork's child reports via its exit code; the parent
// aggregates. Deterministic — blocking IPC (not sleeps) synchronizes
// every step except the kill test's settle delay (kill_test pattern).
// Exit code 0 = all PASS. Launch: start fork_test
// ============================================================
#include "userlib.h"

static int pass = 1;
static int gx = 10;   // data-segment independence probe (cloned by fork)

static void check(char* name, int ok) {
    print(name);
    print(ok ? ": OK\n" : ": FAIL\n");
    if (!ok) pass = 0;
}

void main(void) {
    print("FORK TEST\n");
    int mypid = sys_getpid();
    uint32_t myuid = sys_get_uid();
    int r, status, ok;

    // --- T1: return values, PID/PPID, creds, memory independence ---
    r = sys_fork();
    if (r < 0) { check("fork-basic", 0); sys_exit_code(1); }
    if (r == 0) {
        int okc = (sys_getpid() != mypid) && (sys_getppid() == mypid);
        okc = okc && (sys_get_uid() == myuid);
        okc = okc && (gx == 10);
        gx = 20;   // child's private copy only
        sys_exit_code(okc ? 0 : 1);
    }
    ok = (r > 0) && (r != mypid);
    status = -1;
    ok = ok && (sys_waitpid(r, &status, 0) == r) && (status == 0);
    ok = ok && (gx == 10);
    check("fork-basic", ok);

    // --- T2: fd sharing — same number, shared offset, parent survives ---
    int fd = sys_open("forktest.txt", O_RDWR | O_CREAT | O_TRUNC);
    ok = (fd >= 0) && (sys_write_fd(fd, "PARENT", 6) == 6);
    r = ok ? sys_fork() : -1;
    if (r == 0) {
        // Continues at the SHARED offset 6 (not 0) via the same fd number.
        int okc = (sys_write_fd(fd, "CHILD", 5) == 5);
        sys_close(fd);
        sys_exit_code(okc ? 0 : 1);
    }
    ok = ok && (r > 0);
    status = -1;
    ok = ok && (sys_waitpid(r, &status, 0) == r) && (status == 0);
    char b[16];
    ok = ok && (sys_lseek(fd, 0, SEEK_SET) == 0);
    int n = sys_read_fd(fd, b, 11);
    ok = ok && (n == 11) && (b[0] == 'P') && (b[5] == 'T') && (b[6] == 'C');
    sys_close(fd);
    fs_delete("forktest.txt");
    check("fork-fd", ok);

    // --- T3: pipe inheritance + EOF ---
    int pf[2] = { -1, -1 };
    ok = (sys_pipe(pf) == 0);
    r = ok ? sys_fork() : -1;
    if (r == 0) {
        sys_close(pf[0]);
        int okc = (sys_write_fd(pf[1], "hello", 5) == 5);
        sys_close(pf[1]);
        sys_exit_code(okc ? 0 : 1);
    }
    ok = ok && (r > 0);
    sys_close(pf[1]);   // parent drops write end: EOF follows child's close
    char pb[8];
    ok = ok && (sys_read_fd(pf[0], pb, 5) == 5) && (pb[0] == 'h');
    ok = ok && (sys_read_fd(pf[0], pb, 5) == 0);
    status = -1;
    ok = ok && (sys_waitpid(r, &status, 0) == r) && (status == 0);
    sys_close(pf[0]);
    check("fork-pipe", ok);

    // --- T4: two bounded sequential forks, unique pids, reap both ---
    int c1 = sys_fork();
    if (c1 == 0) sys_exit_code(0);
    int c2 = (c1 > 0) ? sys_fork() : -1;
    if (c2 == 0) sys_exit_code(0);
    ok = (c1 > 0) && (c2 > 0) && (c1 != c2);
    int s1 = -1, s2 = -1;
    ok = ok && (sys_waitpid(c1, &s1, 0) == c1) && (s1 == 0);
    ok = ok && (sys_waitpid(c2, &s2, 0) == c2) && (s2 == 0);
    check("fork-multi", ok);

    // --- T5: kill a blocked forked child through the existing path ---
    int ck = sys_fork();
    if (ck == 0) { for (;;) sys_sleep(1000); }
    sys_sleep(300);   // let the child reach its sleep loop (any CPU)
    ok = (ck > 0) && (sys_kill(ck) == 0);
    status = -1;
    ok = ok && (sys_waitpid(ck, &status, 0) == ck) && (status == 125);
    check("fork-kill", ok);

    // --- T6: failed execve is atomic — old image fully runnable ---
    r = sys_fork();
    if (r == 0) {
        int er = sys_execve("/apps/no_such_app.elf", 0, (char**)0);
        // Must return -1 HERE (same pid, same memory, same fds).
        int okc = (er == -1) && (gx == 10) && (sys_getpid() != mypid);
        char* av[1]; av[0] = "x";
        int er2 = sys_execve("/apps/no_such_app.elf", 1, av);
        okc = okc && (er2 == -1) && (gx == 10);
        sys_exit_code(okc ? 0 : 1);
    }
    ok = (r > 0);
    status = -1;
    ok = ok && (sys_waitpid(r, &status, 0) == r) && (status == 0);
    check("exec-fail", ok);

    // --- T7: successful execve replaces the image, same pid ---
    r = sys_fork();
    if (r == 0) {
        // Become echo: it prints argv to stdout (= console here) and
        // exits 0. Parent reaps the SAME pid it forked.
        char* av[3];
        av[0] = "echo"; av[1] = "execok"; av[2] = (char*)0;
        sys_execve("/apps/echo.elf", 2, av);
        sys_exit_code(127);   // exec failed
    }
    ok = (r > 0);
    status = -1;
    ok = ok && (sys_waitpid(r, &status, 0) == r) && (status == 0);
    check("exec-ok", ok);

    // --- T8: app-driven fork + dup2 + exec pipeline (shell's exact flow) ---
    {
        int pp[2];
        ok = (sys_pipe(pp) == 0);
        int w = ok ? sys_fork() : -1;   // writer stage
        if (w == 0) {
            int okc = (sys_dup2(pp[1], 1) == 1);
            okc = okc && (sys_close(pp[0]) == 0) && (sys_close(pp[1]) == 0);
            if (okc) {
                char* av[3];
                av[0] = "echo"; av[1] = "piped"; av[2] = (char*)0;
                sys_execve("/apps/echo.elf", 2, av);
            }
            sys_exit_code(127);
        }
        int rd = (w > 0) ? sys_fork() : -1;   // reader stage
        if (rd == 0) {
            int okc = (sys_dup2(pp[0], 0) == 0);
            okc = okc && (sys_close(pp[0]) == 0) && (sys_close(pp[1]) == 0);
            if (okc) {
                char* av[2];
                av[0] = "cat"; av[1] = (char*)0;
                sys_execve("/apps/cat.elf", 1, av);
            }
            sys_exit_code(127);
        }
        ok = ok && (w > 0) && (rd > 0) && (w != rd);
        sys_close(pp[0]); sys_close(pp[1]);   // parent holds neither end
        int sw = -1, sr = -1;
        ok = ok && (sys_waitpid(w, &sw, 0) == w) && (sw == 0);
        ok = ok && (sys_waitpid(rd, &sr, 0) == rd) && (sr == 0);
        check("exec-pipe", ok);
    }

    if (pass) print("FORK TEST PASS\n");
    else print("FORK TEST FAIL\n");
    sys_exit_code(pass ? 0 : 1);
}
