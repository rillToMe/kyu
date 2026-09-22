// ============================================================
// procinfo.c — P0 Phase 2 test app: print PID/PPID/UID/GID/argc/argv.
//
// Launch: start procinfo hello world
// Expected:
//   argc: 3
//   argv[0]: procinfo
//   argv[1]: hello
//   argv[2]: world
// Also exercises fd 1 (stdout) via sys_write_fd.
// ============================================================
#include "userlib.h"

static void print_u32(uint32_t n) { print_num(n); }

static void print_i32(int32_t n) {
    if (n < 0) { print("-"); print_u32((uint32_t)(-(int64_t)n)); }
    else print_u32((uint32_t)n);
}

void main(int argc, char* argv[]) {
    int32_t pid  = sys_getpid();
    int32_t ppid = sys_getppid();
    uint32_t uid = sys_get_uid();

    // GID via proc_list snapshot (read-only; find self).
    uint32_t gid = uid;
    {
        proc_info_t list[16];
        int n = sys_proc_list(list, 16);
        for (int i = 0; i < n; i++) {
            if (list[i].pid == pid) { gid = list[i].gid; break; }
        }
    }

    print("PID: ");  print_i32(pid);  print("\n");
    print("PPID: "); print_i32(ppid); print("\n");
    print("UID: ");  print_u32(uid);  print("\n");
    print("GID: ");  print_u32(gid);  print("\n");
    print("argc: "); print_i32(argc); print("\n");
    for (int i = 0; i < argc && i < PROC_MAX_ARGC; i++) {
        print("argv["); print_i32(i); print("]: ");
        print(argv[i] ? argv[i] : "(null)");
        print("\n");
    }

    // Exercise fd 1 (stdout, VFS-backed TTY) alongside syscall-1 print().
    {
        const char* m = "[procinfo: hello via fd 1]\n";
        uint32_t len = 0;
        while (m[len]) len++;
        sys_write_fd(1, m, len);
    }

    sys_exit();
}
