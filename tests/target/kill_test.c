// ============================================================
// kill_test.c — P0 Phase 3 QEMU test: parent kills a blocked child.
//
// Parent spawns itself with argv[1]="child"; the child sleeps in a loop
// (SLEEPING/BLOCKED on some CPU). Parent kills it, waitpids it, and
// verifies: kill ok, waitpid returns child pid with PROC_KILL_EXIT_CODE,
// second waitpid fails, invalid kills fail. Exit code 0 = all PASS.
// Run on 4 CPUs: child usually lands on another CPU than the parent,
// exercising the cross-CPU wake + observe path.
//
// Launch: start kill_test
// ============================================================
#include "userlib.h"

static int stra_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void print_i32(int32_t n) {
    if (n < 0) { print("-"); print_num((uint32_t)(-(int64_t)n)); }
    else print_num((uint32_t)n);
}

void main(int argc, char* argv[]) {
    // --- child: stay alive, blocked in sleep (kill must wake us) ---
    if (argc >= 2 && stra_eq(argv[1], "child")) {
        print("kill_test: child alive pid=");
        print_i32(sys_getpid());
        print("\n");
        for (;;) {
            sys_sleep(1000);
        }
    }

    // --- window child: hold a KWM window + sleep (kill must tear down
    // windows, event queue, address space with no dangling owner) ---
    if (argc >= 2 && stra_eq(argv[1], "winchild")) {
        int wid = sys_create_window(100, 100, 200, 150);
        print("kill_test: winchild alive pid=");
        print_i32(sys_getpid());
        print(" win=");
        print_i32(wid);
        print("\n");
        for (;;) {
            sys_sleep(1000);
        }
    }

    // --- spinchild: pure CPU loop, NO syscalls (P0-FINAL: kill must be
    // observed at timer preemption via proc_observe_kill_sched — there
    // is no other safe boundary for this task).
    if (argc >= 2 && stra_eq(argv[1], "spinchild")) {
        print("kill_test: spinchild alive pid=");
        print_i32(sys_getpid());
        print("\n");
        volatile unsigned long s = 0;
        for (;;) s += 1;
    }

    // --- parent ---
    int pass = 1;
    char app[40];
    build_app_path(app, sizeof(app), "kill_test.elf");
    char* cargv[2];
    cargv[0] = "kill_test";
    cargv[1] = "child";
    int child = sys_spawn_argv(app, 2, cargv);
    print("kill_test: spawned child pid=");
    print_i32(child);
    print("\n");
    if (child < 0) { print("kill_test: FAIL spawn\n"); sys_exit_code(1); }

    sys_sleep(300);   // let the child reach its sleep loop (any CPU)

    // Invalid kills must fail: PID 0, out-of-range, kernel/self-parent chain.
    if (sys_kill(0) != -1) { print("kill_test: FAIL kill(0)\n"); pass = 0; }
    if (sys_kill(99) != -1) { print("kill_test: FAIL kill(99)\n"); pass = 0; }
    if (sys_kill(-2) != -1) { print("kill_test: FAIL kill(-2)\n"); pass = 0; }

    // The real kill.
    if (sys_kill(child) != 0) {
        print("kill_test: FAIL kill(child)\n");
        pass = 0;
    } else {
        print("kill_test: kill ok\n");
    }

    // waitpid must return the child with the kill convention status.
    {
        int status = -999;
        int got = sys_waitpid(child, &status, 0);
        print("kill_test: waitpid=");
        print_i32(got);
        print(" status=");
        print_i32(status);
        print("\n");
        if (got != child) { print("kill_test: FAIL waitpid pid\n"); pass = 0; }
        if (status != PROC_KILL_EXIT_CODE) {
            print("kill_test: FAIL status != KILL\n");
            pass = 0;
        }
    }

    // Second waitpid on the same PID must fail (reaped exactly once).
    {
        int status = 0;
        if (sys_waitpid(child, &status, 0) != -1) {
            print("kill_test: FAIL double reap\n");
            pass = 0;
        }
    }

    // Window child: kill must destroy its KWM window without tripping the
    // compositor or leaving a dangling owner (same destroy path as exit).
    {
        char* wargv[2];
        wargv[0] = "kill_test";
        wargv[1] = "winchild";
        int wchild = sys_spawn_argv(app, 2, wargv);
        if (wchild < 0) { print("kill_test: FAIL winspawn\n"); pass = 0; }
        else {
            sys_sleep(500);   // let it create its window on any CPU
            if (sys_kill(wchild) != 0) {
                print("kill_test: FAIL winkill\n");
                pass = 0;
            }
            int status = -999;
            int got = sys_waitpid(wchild, &status, 0);
            if (got != wchild || status != PROC_KILL_EXIT_CODE) {
                print("kill_test: FAIL winwait ");
                print_i32(got);
                print(" ");
                print_i32(status);
                print("\n");
                pass = 0;
            } else {
                print("kill_test: winkill ok\n");
            }
        }
    }

    // CPU-bound kill: a task spinning in userspace with NO syscalls can
    // only observe kill at timer preemption (proc_observe_kill_sched).
    // Single victim first (status must be the kill convention).
    {
        char* sargv[2];
        sargv[0] = "kill_test";
        sargv[1] = "spinchild";
        int sc = sys_spawn_argv(app, 2, sargv);
        if (sc < 0) { print("kill_test: FAIL spinspawn\n"); pass = 0; }
        else {
            sys_sleep(500);   // let it start spinning on any CPU
            if (sys_kill(sc) != 0) {
                print("kill_test: FAIL spinkill\n");
                pass = 0;
            }
            int status = -999;
            int got = sys_waitpid(sc, &status, 0);
            if (got != sc || status != PROC_KILL_EXIT_CODE) {
                print("kill_test: FAIL spinwait ");
                print_i32(got);
                print(" ");
                print_i32(status);
                print("\n");
                pass = 0;
            } else {
                print("kill_test: spinkill ok\n");
            }
        }
    }

    // Multiple CPU-bound victims + one sleeping victim killed in one burst.
    {
        int v[3];
        int i;
        for (i = 0; i < 3; i++) {
            char* vargv[2];
            vargv[0] = "kill_test";
            vargv[1] = (i < 2) ? "spinchild" : "child";
            v[i] = sys_spawn_argv(app, 2, vargv);
            if (v[i] < 0) { print("kill_test: FAIL multispawn\n"); pass = 0; }
        }
        sys_sleep(500);
        for (i = 0; i < 3; i++) {
            if (v[i] < 0) continue;
            if (sys_kill(v[i]) != 0) {
                print("kill_test: FAIL multikill ");
                print_i32(i);
                print("\n");
                pass = 0;
            }
        }
        for (i = 0; i < 3; i++) {
            if (v[i] < 0) continue;
            int status = -999;
            int got = sys_waitpid(v[i], &status, 0);
            if (got != v[i] || status != PROC_KILL_EXIT_CODE) {
                print("kill_test: FAIL multiwait ");
                print_i32(i);
                print("\n");
                pass = 0;
            }
        }
        if (pass) print("kill_test: multikill ok\n");
    }

    // Killing the reaped (now possibly reused-or-dead) PID as non-child
    // must not succeed against a live unrelated task; at minimum it must
    // not report success for the already-reaped zombie.
    // (Deterministic part: our own dead child id is no longer ours.)
    {
        int status = 0;
        if (sys_waitpid(child, &status, 0) != -1) {
            print("kill_test: FAIL stale wait\n");
            pass = 0;
        }
    }

    if (pass) print("kill_test: PASS\n");
    else print("kill_test: FAIL\n");
    sys_exit_code(pass ? 0 : 1);
}
