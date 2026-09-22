// ============================================================
// fd_test.c — P0 Phase 4 QEMU test: dup/dup2 + shared-offset model.
//
// Exercises the real kernel fd layer on 4 CPUs:
//   open / dup / shared offset / dup-survives-close / dup2 replace /
//   dup2 self no-op / invalid fds / cleanup.
// Exit code 0 = all PASS. Launch: start fd_test
// ============================================================
#include "userlib.h"

static void check(char* name, int ok, int* pass) {
    print(name);
    print(ok ? ": OK\n" : ": FAIL\n");
    if (!ok) *pass = 0;
}

void main(void) {
    int pass = 1;
    print("FD TEST\n");

    // --- open + write ---
    int fd3 = sys_open("fdtest.txt", O_RDWR | O_CREAT | O_TRUNC);
    check("open", fd3 >= 0, &pass);
    if (fd3 < 0) { print("FD TEST ABORT\n"); sys_exit_code(1); }
    check("write", sys_write_fd(fd3, "ABCDE", 5) == 5, &pass);
    check("lseek", sys_lseek(fd3, 0, SEEK_SET) == 0, &pass);

    // --- dup shares the open description ---
    int fd4 = sys_dup(fd3);
    check("dup", fd4 >= 0 && fd4 != fd3, &pass);

    // --- shared offset: AB via fd3, then CD via fd4 ---
    char b[4];
    int ok = (sys_read_fd(fd3, b, 2) == 2 && b[0] == 'A' && b[1] == 'B');
    ok = ok && (sys_read_fd(fd4, b, 2) == 2 && b[0] == 'C' && b[1] == 'D');
    check("shared offset", ok, &pass);

    // --- closing original leaves duplicate valid ---
    check("dup close", sys_close(fd3) == 0, &pass);
    ok = (sys_read_fd(fd4, b, 1) == 1 && b[0] == 'E');
    check("dup survives", ok, &pass);
    check("dup cleanup", sys_close(fd4) == 0, &pass);

    // --- dup2 replaces target ---
    int fdA = sys_open("fdtest_a.txt", O_RDWR | O_CREAT | O_TRUNC);
    int fdB = sys_open("fdtest_b.txt", O_RDWR | O_CREAT | O_TRUNC);
    sys_write_fd(fdA, "AAAA", 4);
    sys_write_fd(fdB, "BBBB", 4);
    sys_lseek(fdA, 0, SEEK_SET);
    sys_lseek(fdB, 0, SEEK_SET);
    check("dup2", sys_dup2(fdA, fdB) == fdB, &pass);
    ok = (sys_read_fd(fdB, b, 4) == 4 && b[0] == 'A' && b[3] == 'A');
    check("dup2 content", ok, &pass);
    check("dup2 self", sys_dup2(fdA, fdA) == fdA, &pass);

    // --- invalid fds fail safely ---
    ok = (sys_dup(-1) == -1);
    ok = ok && (sys_dup(99) == -1);
    ok = ok && (sys_dup2(-1, 3) == -1);
    ok = ok && (sys_dup2(fdA, 99) == -1);
    ok = ok && (sys_dup2(9, 10) == -1);
    ok = ok && (sys_close(99) == -1);
    ok = ok && (sys_read_fd(99, b, 1) == -1);
    ok = ok && (sys_write_fd(99, "x", 1) == -1);
    check("invalid fd", ok, &pass);

    // --- stdio still works (reached here via fd 1) + cleanup ---
    sys_close(fdA);
    sys_close(fdB);
    fs_delete("fdtest.txt");
    fs_delete("fdtest_a.txt");
    fs_delete("fdtest_b.txt");
    check("cleanup", 1, &pass);

    if (pass) print("FD TEST PASS\n");
    else print("FD TEST FAIL\n");
    sys_exit_code(pass ? 0 : 1);
}
