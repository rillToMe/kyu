// ============================================================
// cat.c — external cat for pipelines/redirection (P0 Phase 5).
//
// The `cat` shell builtin reads whole files in-process and cannot read
// stdin — this external app pumps fd 0 -> fd 1 (no args) or each named
// file -> fd 1, so `a | cat`, `cat < f`, and `cat f > out` work.
// Launch: start cat [file...]
// ============================================================
#include "userlib.h"

static int pump(int infd) {
    char b[64];
    for (;;) {
        int n = sys_read_fd(infd, b, sizeof(b));
        if (n < 0) return -1;
        if (n == 0) return 0;   // EOF
        int w = 0;
        while (w < n) {
            int m = sys_write_fd(1, b + w, (uint32_t)(n - w));
            if (m <= 0) return -1;
            w += m;
        }
    }
}

void main(int argc, char* argv[]) {
    int rc = 0;
    if (argc < 2) {
        rc = pump(0);
    } else {
        for (int i = 1; i < argc; i++) {
            int fd = sys_open(argv[i], O_RDONLY);
            if (fd < 0) { rc = 1; continue; }
            if (pump(fd) != 0) rc = 1;
            sys_close(fd);
        }
    }
    sys_exit_code(rc);
}
