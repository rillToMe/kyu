// ============================================================
// exit_test.c — P0 Phase 2 test app: exit with a caller-supplied code.
//
// Launch: start exit_test 42  (then parent waitpid() receives 42)
// Usage: exit_test <code>  (0..255; default 0)
// ============================================================
#include "userlib.h"

static int parse_u32(const char* s, uint32_t* out) {
    if (!s || !out) return 0;
    while (*s == ' ') s++;
    if (!*s) return 0;
    uint32_t v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    *out = v;
    return 1;
}

void main(int argc, char* argv[]) {
    uint32_t code = 0;
    if (argc >= 2) {
        uint32_t v = 0;
        if (parse_u32(argv[1], &v)) code = v & 0xFF;
        else code = 1;
    }
    print("exit_test: exiting with code ");
    print_num(code);
    print("\n");
    sys_exit_code((int)code);
}
