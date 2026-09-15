// ============================================================
// echo.c — external echo for pipelines/redirection (P0 Phase 5).
//
// The `echo` shell builtin runs in-process (shell_io) and cannot take
// part in fd redirection — this external app writes argv to fd 1, so
// `echo hello | cat` and `echo hello > f` work through real FDs.
// Launch: start echo hello world
// ============================================================
#include "userlib.h"

void main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        const char* s = argv[i];
        int n = 0;
        while (s[n]) n++;
        if (n > 0) sys_write_fd(1, s, (uint32_t)n);
        if (i + 1 < argc) sys_write_fd(1, " ", 1);
    }
    sys_write_fd(1, "\n", 1);
    sys_exit_code(0);
}
