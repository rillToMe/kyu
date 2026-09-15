// Host-side unit test: per-task credential policy (P0 Phase 1).
//
// Tests the pure policy in include/cred.h WITHOUT the scheduler:
//   1. root task starts (0, 0)
//   2. user task holds its own uid
//   3. child inherits parent uid/gid exactly
//   4. mutating one cred never touches another (isolation)
//   5. non-root sys_set_uid transition is denied
//   6. root sys_set_uid transition is allowed (uid+gid move together)
//   7. non-root privileged-op check fails, root passes
//
// Scheduler integration (real tasks, SMP stability, shell whoami/id,
// QEMU boot/login/desktop/terminal) is covered by QEMU regression,
// not here: tasks[]/smp lapic cannot run on host.
//
// Build: clang -iquote include test/cred_test.c -o test/cred_test
// Run:   ./test/cred_test  (or: make test-cred)

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "cred.h"

// Mirror of the syscall-27 transition body (kernel/syscall.c + Ring-0
// shim in apps/kernel_userlib.c): root-only, uid+gid move together.
// The test pins the POLICY; QEMU pins the real task wiring.
static int policy_set_uid(cred_t* self, uint32_t caller_uid, uint32_t want) {
    (void)self;
    if (!cred_transition_allowed(caller_uid)) return -1;
    self->uid = want;
    self->gid = want;
    return 0;
}

int main(void) {
    // TEST 1: root task starts with UID 0.
    cred_t root = { CRED_ROOT_UID, CRED_ROOT_GID };
    assert(root.uid == 0 && root.gid == 0);
    assert(cred_is_root(&root));

    // TEST 2: normal user task starts with its own UID.
    cred_t user = { 1000, 1000 };
    assert(user.uid == 1000 && !cred_is_root(&user));

    // TEST 3: child inherits parent UID/GID.
    cred_t child = { 0xDEAD, 0xBEEF };
    cred_inherit(&child, &user);
    assert(child.uid == 1000 && child.gid == 1000);

    // TEST 4: credentials are values, not aliases.
    child.uid = 1001; child.gid = 1001;
    assert(user.uid == 1000 && user.gid == 1000);
    assert(child.uid == 1001);

    // TEST 5: normal user cannot become UID 0 through sys_set_uid.
    cred_t attacker = { 1000, 1000 };
    assert(policy_set_uid(&attacker, attacker.uid, 0) == -1);
    assert(attacker.uid == 1000 && attacker.gid == 1000);

    // TEST 6: root can perform the transition (login drop / sudo restore).
    cred_t session = { 0, 0 };
    assert(policy_set_uid(&session, session.uid, 1000) == 0);
    assert(session.uid == 1000 && session.gid == 1000);
    assert(policy_set_uid(&session, 0, 0) == 0);  // root restores
    assert(session.uid == 0);

    // TEST 7: privileged-op gate (format/shutdown/reboot pattern).
    assert(!cred_is_root(&attacker));  // denied
    assert(cred_is_root(&root));       // allowed

    // Null-safety: helpers never crash on bad input.
    cred_inherit(NULL, &user);
    cred_inherit(&child, NULL);
    assert(!cred_is_root(NULL));
    assert(!cred_transition_allowed(1000));
    assert(cred_transition_allowed(0));

    printf("[cred] all 7 policy tests passed\n");
    return 0;
}
