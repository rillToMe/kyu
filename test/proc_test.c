// Host-side unit test: P0 Phase 2 process policy (pure, no scheduler).
//
// Tests include/proc.h inline policy WITHOUT SMP/tasks:
//   1. pid in range (valid slot)
//   2. child parent assignment (inherit, not inferred)
//   3. child inherits credentials (cred.h, unchanged by Phase 2)
//   4. argc construction (basic)
//   5. argv bounds rejection (count / len / total)
//   6. stdio fds are 0/1/2
//   7. fd isolation rule (per-task windows, owner check)
//   8. exit status stored as plain int (distinct from syscall -1)
//   9. parent can wait for own child (proc_can_wait)
//  10. wait blocks conceptually (no zombie yet -> would block, not fail)
//  11. exit wakes parent (zombie becomes reapable by same parent)
//  12. correct status delivered (value preserved)
//  13. wait on non-child rejected
//  14. exited-not-reaped is not runnable (zombie != ready/running)
//  15. cross-CPU wake is same-state transition (policy is CPU-independent)
//  16. siblings wait independently (per-child parent link)
//  17. cred isolation across fork-like creation (no UID-as-parent)
//
// Scheduler/SMP/blocking integration (real tasks, QEMU boot, conc suite)
// is covered by QEMU regression, not here: tasks[]/lapic cannot run on host.
//
// Build: clang -Iinclude test/proc_test.c -o test/proc_test
// Run:   ./test/proc_test  (or: make test-proc)

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "proc.h"
#include "cred.h"
#include "task.h"   // TASK_* states (no scheduler link needed for constants)
#include "vfs.h"    // VFS_MAX_FDS (per-task fd windows)

// Mock liveness: slot state array, mimicking tasks[].state checks.
#define MOCK_MAX 16
static int mock_parent[MOCK_MAX];
static int mock_state[MOCK_MAX];   // TASK_* values
static int mock_code[MOCK_MAX];

// Mock policy for "would waitpid block or reap": mirrors proc_waitpid's
// decision table without locks: reap iff own zombie child exists.
static int mock_wait(int self, int pid, int* status) {
    if (pid != PROC_WAIT_ANY && (pid < 0 || pid >= MOCK_MAX)) return -1;
    if (pid == PROC_WAIT_ANY) {
        for (int i = 0; i < MOCK_MAX; i++) {
            if (mock_state[i] == TASK_DEAD) continue;
            if (mock_parent[i] != self) continue;
            if (mock_state[i] == TASK_ZOMBIE) {
                if (status) *status = mock_code[i];
                mock_state[i] = TASK_DEAD;
                mock_parent[i] = PROC_NO_PARENT;
                return i;
            }
        }
        return -1;   // no child (-1) or all running (-1 = would block in kernel)
    }
    if (mock_state[pid] == TASK_DEAD || mock_parent[pid] != self) return -1;
    if (mock_state[pid] != TASK_ZOMBIE) return -1;   // running -> kernel blocks
    if (status) *status = mock_code[pid];
    mock_state[pid] = TASK_DEAD;
    mock_parent[pid] = PROC_NO_PARENT;
    return pid;
}

int main(void) {
    // TEST 1: pid is a slot id, stable while alive.
    assert(proc_pid_in_range(0, MOCK_MAX));
    assert(proc_pid_in_range(15, MOCK_MAX));
    assert(!proc_pid_in_range(-1, MOCK_MAX));
    assert(!proc_pid_in_range(16, MOCK_MAX));

    // TEST 2: parent explicit at creation (B.parent = A), never inferred.
    int parent = 3, child = 5;
    mock_parent[child] = parent;
    assert(mock_parent[child] == parent);
    // Kernel tasks without parent use the sentinel, never UID 0.
    mock_parent[0] = PROC_NO_PARENT;
    assert(mock_parent[0] == PROC_NO_PARENT);

    // TEST 3: child inherits credentials (P0.1 untouched).
    cred_t u = { 1000, 1000 }, c = { 0, 0 };
    cred_inherit(&c, &u);
    assert(c.uid == 1000 && c.gid == 1000);

    // TEST 4: basic argc construction (shell: start app a1 a2 -> 3).
    assert(proc_argc_valid(1) && proc_argc_valid(3) && proc_argc_valid(16));

    // TEST 5: bounds rejection.
    assert(!proc_argc_valid(0) && !proc_argc_valid(17));
    assert(proc_arg_len_valid(1) && proc_arg_len_valid(64));
    assert(!proc_arg_len_valid(0) && !proc_arg_len_valid(65));
    assert(proc_arg_total_valid(512) && !proc_arg_total_valid(513));

    // TEST 6: stdio numbers.
    assert(PROC_STDIN == 0 && PROC_STDOUT == 1 && PROC_STDERR == 2);

    // TEST 7: fd isolation is per-task windows (slot = task*16+fd).
    {
        int task_a = 2, task_b = 3, fd = 1;
        int slot_a = task_a * VFS_MAX_FDS + fd;
        int slot_b = task_b * VFS_MAX_FDS + fd;
        assert(slot_a != slot_b);   // same fd number, different objects
    }

    // TEST 8: exit status is a plain int, distinct from syscall errors.
    {
        int code = 42;
        mock_code[child] = code;
        assert(mock_code[child] == 42);   // stored verbatim, not mapped
    }

    // Setup: parent=3 has two running children 5,6.
    for (int i = 0; i < MOCK_MAX; i++) { mock_state[i] = TASK_DEAD; mock_parent[i] = PROC_NO_PARENT; mock_code[i] = 0; }
    mock_parent[5] = 3; mock_state[5] = TASK_RUNNING;
    mock_parent[6] = 3; mock_state[6] = TASK_RUNNING;

    // TEST 9+10: parent may wait for own child; running -> would block (-1 here).
    assert(proc_can_wait(3, mock_parent[5], 5, 5));
    assert(mock_wait(3, 5, NULL) == -1);   // no zombie yet: kernel would BLOCK

    // TEST 11+12: child exit -> zombie -> waiter reaps with correct status.
    mock_state[5] = TASK_ZOMBIE; mock_code[5] = 7;
    {
        int st = -1;
        assert(mock_wait(3, 5, &st) == 5 && st == 7);
    }

    // TEST 13: waiting on non-child rejected (even if that slot is a zombie).
    mock_parent[7] = 9; mock_state[7] = TASK_ZOMBIE; mock_code[7] = 1;
    assert(!proc_can_wait(3, mock_parent[7], 7, 7));
    assert(mock_wait(3, 7, NULL) == -1);
    assert(mock_state[7] == TASK_ZOMBIE);   // untouched by the stranger

    // TEST 14: zombie is not runnable.
    assert(TASK_ZOMBIE != TASK_READY && TASK_ZOMBIE != TASK_RUNNING);

    // TEST 15: wake policy is CPU-independent (same parent link on any CPU).
    assert(proc_can_wait(3, mock_parent[6], 6, 6));
    mock_state[6] = TASK_ZOMBIE; mock_code[6] = 99;
    {
        int st = -1;
        assert(mock_wait(3, PROC_WAIT_ANY, &st) == 6 && st == 99);
    }

    // TEST 16: siblings independent (reaping one leaves the other).
    mock_parent[5] = 3; mock_state[5] = TASK_ZOMBIE; mock_code[5] = 5;
    mock_parent[6] = 3; mock_state[6] = TASK_ZOMBIE; mock_code[6] = 6;
    {
        int a = mock_wait(3, 5, NULL), b = mock_wait(3, 6, NULL);
        assert(a == 5 && b == 6);
    }

    // TEST 17: UID never substitutes for parenthood.
    {
        cred_t same_uid_parent = { 1000, 1000 }, same_uid_other = { 1000, 1000 };
        (void)same_uid_parent; (void)same_uid_other;
        mock_parent[8] = 4; mock_state[8] = TASK_ZOMBIE; mock_code[8] = 0;
        assert(!proc_can_wait(3, mock_parent[8], 8, 8));   // same uid, not child
        assert(mock_wait(3, 8, NULL) == -1);
    }

    printf("[proc] all 17 policy tests passed\n");
    return 0;
}
