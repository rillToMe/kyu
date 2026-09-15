// Host-side unit test: P0 Phase 3 kill policy + lifecycle (pure, no SMP).
//
// Pins include/proc.h policy WITHOUT tasks/lapic:
//   1. parent can kill own child
//   2. unrelated user cannot kill another's child
//   3. root can kill any spawned task
//   4. invalid PID rejected (range + dead slot + zombie)
//   5. PID 0 / kernel tasks unkillable (even by root)
//   6. self-kill allowed for spawned tasks only
//   7. killed child cannot run again (READY purged -> terminal state)
//   8. killed child reaches ZOMBIE (live parent) / DEAD (reaper parent)
//   9. waitpid receives killed child with PROC_KILL_EXIT_CODE
//  10. exit_reason distinguishes kill (KILLED) from normal exit (NORMAL)
//  11. waitpid cannot reap the same child twice
//  12. killed BLOCKED task wakes (flag + unblock -> observes -> exits)
//  13. killed READY task removed safely (sync path, never runs again)
//  14. double kill does not double-cleanup (one transition wins)
//  15. orphaned children reparent to task 0 and stay valid
//  16. PID slot reusable only after reap
//  17. stale PID wait is deterministic (-1, never a new process)
//  18. UID never substitutes for parenthood (same-uid stranger denied)
//
// SMP/runqueue/IPI integration (real tasks, 4-CPU boot) is covered by the
// QEMU kill_test app, not here: tasks[]/lapic cannot run on host.
//
// Build: clang -Iinclude test/kill_test.c -o test/kill_test
// Run:   ./test/kill_test  (or: make test-kill)

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "proc.h"
#include "cred.h"
#include "task.h"   // TASK_* states + TASK_KIND_* (constants only)

#define MOCK_MAX 16

static int mock_state[MOCK_MAX];    // TASK_* values
static int mock_parent[MOCK_MAX];
static int mock_kind[MOCK_MAX];     // TASK_KIND_*
static int mock_pending[MOCK_MAX];  // kill_pending
static int mock_code[MOCK_MAX];
static int mock_reason[MOCK_MAX];
static int mock_queued[MOCK_MAX];   // READY + present in a run queue
static int mock_cleanups;           // authoritative transitions performed

static void mock_reset(void) {
    for (int i = 0; i < MOCK_MAX; i++) {
        mock_state[i] = TASK_DEAD;
        mock_parent[i] = PROC_NO_PARENT;
        mock_kind[i] = TASK_KIND_KERNEL;
        mock_pending[i] = 0;
        mock_code[i] = 0;
        mock_reason[i] = PROC_EXIT_NORMAL;
        mock_queued[i] = 0;
    }
    mock_cleanups = 0;
}

// One authoritative transition (mirrors proc_transition_locked): exactly one
// winner per task; returns 1 on success, 0 if already ZOMBIE/DEAD.
static int mock_transition(int id, int code, int reason) {
    if (id < 0 || id >= MOCK_MAX) return 0;
    if (mock_state[id] == TASK_ZOMBIE || mock_state[id] == TASK_DEAD) return 0;
    // reparent children to task 0
    for (int i = 0; i < MOCK_MAX; i++) {
        if (mock_state[i] != TASK_DEAD && mock_parent[i] == id) {
            mock_parent[i] = (id != 0) ? 0 : PROC_NO_PARENT;
        }
    }
    int p = mock_parent[id];
    int parent_alive = (p > 0 && p < MOCK_MAX &&
                        mock_state[p] != TASK_DEAD &&
                        mock_state[p] != TASK_ZOMBIE);
    mock_state[id] = parent_alive ? TASK_ZOMBIE : TASK_DEAD;
    if (mock_state[id] == TASK_DEAD) mock_parent[id] = PROC_NO_PARENT;
    mock_code[id] = code;
    mock_reason[id] = reason;
    mock_pending[id] = 0;
    mock_queued[id] = 0;
    mock_cleanups++;
    return 1;
}

// Mirror of proc_kill's decision table. is_root/self model the caller.
// Returns 0 ok, -1 denied. SYNC (READY) transitions immediately; otherwise
// flags the target (async) — the caller must run mock_observe() to model
// the target reaching its next safe boundary.
static int mock_kill(int is_root, int self, int pid) {
    if (!proc_pid_in_range(pid, MOCK_MAX)) return -1;
    int alive = (mock_state[pid] != TASK_DEAD &&
                 mock_state[pid] != TASK_ZOMBIE);
    int spawned = (mock_kind[pid] == TASK_KIND_SPAWNED);
    if (!proc_can_kill(is_root, self, mock_parent[pid], pid, spawned, alive))
        return -1;
    if (pid == self) return mock_transition(pid, PROC_KILL_EXIT_CODE,
                                            PROC_EXIT_KILLED) ? 0 : -1;
    if (mock_pending[pid]) return 0;   // double kill: already requested
    if (mock_state[pid] == TASK_READY) {
        mock_queued[pid] = 0;          // scheduler_remove_task purge
        return mock_transition(pid, PROC_KILL_EXIT_CODE,
                               PROC_EXIT_KILLED) ? 0 : 0;
    }
    mock_pending[pid] = 1;             // async: wake + observe later
    if (mock_state[pid] == TASK_BLOCKED || mock_state[pid] == TASK_SLEEPING) {
        mock_state[pid] = TASK_RUNNING;   // unblock_task wake
    }
    return 0;
}

// Target reaches a safe boundary (wait-path re-lock / sleep return /
// syscall entry): self-removes from its queue and exits exactly once.
static void mock_observe(int pid) {
    if (pid < 0 || pid >= MOCK_MAX) return;
    if (!mock_pending[pid]) return;
    mock_transition(pid, PROC_KILL_EXIT_CODE, PROC_EXIT_KILLED);
}

// Mirror of proc_waitpid reap-once (parent-only, zombie -> DEAD).
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
        return -1;
    }
    if (mock_state[pid] == TASK_DEAD || mock_parent[pid] != self) return -1;
    if (mock_state[pid] != TASK_ZOMBIE) return -1;
    if (status) *status = mock_code[pid];
    mock_state[pid] = TASK_DEAD;
    mock_parent[pid] = PROC_NO_PARENT;
    return pid;
}

int main(void) {
    // TEST 1: parent can kill own child.
    mock_reset();
    mock_state[3] = TASK_RUNNING; mock_kind[3] = TASK_KIND_SPAWNED;
    mock_state[5] = TASK_RUNNING; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 3;
    assert(proc_can_kill(0, 3, mock_parent[5], 5, 1, 1));
    assert(mock_kill(0, 3, 5) == 0 && mock_pending[5] == 1);

    // TEST 2: unrelated user cannot kill another's child.
    assert(!proc_can_kill(0, 7, mock_parent[5], 5, 1, 1));
    assert(mock_kill(0, 7, 5) == -1);

    // TEST 3: root can kill any spawned task.
    assert(proc_can_kill(1, 7, mock_parent[5], 5, 1, 1));
    mock_reset();
    mock_state[7] = TASK_RUNNING; mock_kind[7] = TASK_KIND_SPAWNED;
    mock_state[5] = TASK_BLOCKED; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 3;
    assert(mock_kill(1, 7, 5) == 0);

    // TEST 4: invalid PID rejected (range, dead slot, zombie).
    assert(!proc_pid_in_range(-1, MOCK_MAX));
    assert(!proc_pid_in_range(16, MOCK_MAX));
    assert(mock_kill(1, 0, -1) == -1 && mock_kill(1, 0, 99) == -1);
    mock_reset();
    mock_state[5] = TASK_ZOMBIE; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 3; mock_code[5] = PROC_KILL_EXIT_CODE;
    assert(mock_kill(1, 0, 5) == -1);   // already exited: no second cleanup
    mock_state[6] = TASK_DEAD;
    assert(mock_kill(1, 0, 6) == -1);

    // TEST 5: PID 0 / kernel tasks unkillable, even by root.
    mock_reset();
    mock_state[0] = TASK_RUNNING; mock_kind[0] = TASK_KIND_KERNEL;
    mock_parent[0] = PROC_NO_PARENT;
    assert(!proc_can_kill(1, 7, PROC_NO_PARENT, 0, 0, 1));
    assert(mock_kill(1, 7, 0) == -1);
    mock_state[4] = TASK_RUNNING; mock_kind[4] = TASK_KIND_KERNEL;
    mock_parent[4] = 3;
    assert(!proc_can_kill(1, 0, 3, 4, 0, 1));   // root, but kernel kind
    assert(mock_kill(1, 0, 4) == -1);

    // TEST 6: self-kill allowed for spawned tasks, denied for kernel.
    mock_reset();
    mock_state[3] = TASK_RUNNING; mock_kind[3] = TASK_KIND_SPAWNED;
    mock_state[5] = TASK_RUNNING; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 3;
    assert(proc_can_kill(0, 5, 3, 5, 1, 1));
    assert(mock_kill(0, 5, 5) == 0);   // exits directly (zombie: parent 3 live)
    assert(mock_state[5] == TASK_ZOMBIE);
    mock_reset();
    mock_state[2] = TASK_RUNNING; mock_kind[2] = TASK_KIND_KERNEL;
    assert(!proc_can_kill(0, 2, 0, 2, 0, 1));
    assert(mock_kill(0, 2, 2) == -1 && mock_state[2] == TASK_RUNNING);

    // TEST 7+13: killed READY child purged synchronously, never runs again.
    mock_reset();
    mock_state[3] = TASK_RUNNING; mock_kind[3] = TASK_KIND_SPAWNED;
    mock_state[5] = TASK_READY; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 3; mock_queued[5] = 1;
    assert(mock_kill(0, 3, 5) == 0);
    assert(mock_queued[5] == 0);                       // purged from runqueue
    assert(mock_state[5] != TASK_READY &&
           mock_state[5] != TASK_RUNNING);             // cannot run again
    assert(mock_pending[5] == 0);                      // no dangling request

    // TEST 8: terminal state — ZOMBIE with live parent, DEAD with reaper.
    assert(mock_state[5] == TASK_ZOMBIE);               // parent 3 alive
    mock_reset();
    mock_state[5] = TASK_READY; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 0; mock_queued[5] = 1;             // reaper placeholder
    assert(mock_kill(1, 0, 5) == 0);
    assert(mock_state[5] == TASK_DEAD);                 // auto-reaped, no zombie

    // TEST 9+10: waitpid receives killed child; reason distinguishes kill.
    mock_reset();
    mock_state[3] = TASK_RUNNING; mock_kind[3] = TASK_KIND_SPAWNED;
    mock_state[5] = TASK_RUNNING; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 3;
    assert(mock_kill(0, 3, 5) == 0);
    mock_observe(5);                                    // safe boundary
    assert(mock_state[5] == TASK_ZOMBIE);
    assert(mock_code[5] == PROC_KILL_EXIT_CODE);
    assert(mock_reason[5] == PROC_EXIT_KILLED);
    {
        int st = -1;
        assert(mock_wait(3, 5, &st) == 5 && st == PROC_KILL_EXIT_CODE);
    }
    // Normal exit keeps NORMAL reason (plain code untouched).
    mock_state[6] = TASK_RUNNING; mock_kind[6] = TASK_KIND_SPAWNED;
    mock_parent[6] = 3;
    assert(mock_transition(6, 42, PROC_EXIT_NORMAL));
    assert(mock_code[6] == 42 && mock_reason[6] == PROC_EXIT_NORMAL);

    // TEST 11: cannot reap the same child twice.
    assert(mock_wait(3, 5, NULL) == -1);

    // TEST 12: killed BLOCKED task wakes and exits (no permanent block).
    mock_reset();
    mock_state[3] = TASK_RUNNING; mock_kind[3] = TASK_KIND_SPAWNED;
    mock_state[5] = TASK_BLOCKED; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 3;
    assert(mock_kill(0, 3, 5) == 0);
    assert(mock_state[5] == TASK_RUNNING);              // unblock woke it
    mock_observe(5);                                    // wait-path observation
    assert(mock_state[5] == TASK_ZOMBIE &&
           mock_code[5] == PROC_KILL_EXIT_CODE);

    // TEST 14: double kill does not double-cleanup.
    mock_reset();
    mock_state[3] = TASK_RUNNING; mock_kind[3] = TASK_KIND_SPAWNED;
    mock_state[5] = TASK_RUNNING; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 3;
    assert(mock_kill(0, 3, 5) == 0);
    assert(mock_kill(0, 3, 5) == 0);   // idempotent while pending
    mock_observe(5);
    assert(mock_cleanups == 1);        // exactly one transition
    assert(mock_kill(0, 3, 5) == -1);  // zombie: rejected, no re-cleanup
    assert(mock_cleanups == 1);

    // TEST 15: orphaned children reparent to task 0 and stay valid.
    mock_reset();
    mock_state[3] = TASK_RUNNING; mock_kind[3] = TASK_KIND_SPAWNED;
    mock_parent[3] = 0;
    mock_state[5] = TASK_SLEEPING; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 3;
    mock_state[6] = TASK_READY; mock_kind[6] = TASK_KIND_SPAWNED;
    mock_parent[6] = 3; mock_queued[6] = 1;
    assert(mock_kill(1, 0, 3) == 0);   // kill the parent (async: RUNNING)
    mock_observe(3);
    assert(mock_parent[5] == 0 && mock_parent[6] == 0);   // reparented
    assert(mock_state[5] == TASK_SLEEPING && mock_state[6] == TASK_READY);

    // TEST 16: PID slot reusable only after reap.
    mock_reset();
    mock_state[3] = TASK_RUNNING; mock_kind[3] = TASK_KIND_SPAWNED;
    mock_state[5] = TASK_RUNNING; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 3;
    assert(mock_kill(0, 3, 5) == 0);
    mock_observe(5);
    assert(mock_state[5] == TASK_ZOMBIE);   // slot still occupied
    {
        int st = 0;
        assert(mock_wait(3, 5, &st) == 5);   // reap frees it
    }
    assert(mock_state[5] == TASK_DEAD && mock_parent[5] == PROC_NO_PARENT);

    // TEST 17: stale PID wait is deterministic (never a new process).
    mock_state[5] = TASK_RUNNING; mock_kind[5] = TASK_KIND_SPAWNED;
    mock_parent[5] = 9;   // slot reused by ANOTHER parent's child
    assert(mock_wait(3, 5, NULL) == -1);   // not our child anymore

    // TEST 18: UID never substitutes for parenthood.
    mock_reset();
    mock_state[3] = TASK_RUNNING; mock_kind[3] = TASK_KIND_SPAWNED;
    mock_state[8] = TASK_RUNNING; mock_kind[8] = TASK_KIND_SPAWNED;
    mock_parent[8] = 4;   // same uid as 3 is irrelevant
    assert(!proc_can_kill(0, 3, mock_parent[8], 8, 1, 1));
    assert(mock_kill(0, 3, 8) == -1 && mock_state[8] == TASK_RUNNING);

    printf("[kill] all 18 policy/lifecycle tests passed\n");
    return 0;
}
