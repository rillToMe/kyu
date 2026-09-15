#ifndef CRED_H
#define CRED_H

#include <stdint.h>

// ============================================================
// P0 Phase 1 — Per-task credentials.
//
// Kernel-owned identity attached to task_t (see task.h). One uid +
// one gid. No groups, capabilities, or ACLs in this phase.
//
// Lifecycle:
//   tasking_init ......... task0 = (0, 0) kernel/root identity.
//   create_task(_prio) .... inherit creator cred (boot = root).
//   create_user_task ...... inherit syscall caller cred; fail (-1)
//                           if caller cannot be determined.
//   sys_set_uid ........... root-only transition, sets uid+gid.
//                           Non-root is denied, never elevated.
//
// KWM/filesystem ownership stays task-id based; uid/gid gates only
// the privileged syscalls listed in syscall.c (format/shutdown/
// reboot/set_uid). Full permission model is a later P0 phase.
// ============================================================

typedef struct {
    uint32_t uid;
    uint32_t gid;
} cred_t;

#define CRED_ROOT_UID ((uint32_t)0)
#define CRED_ROOT_GID ((uint32_t)0)

// Child starts as an exact copy of the parent. No transformation.
static inline void cred_inherit(cred_t* child, const cred_t* parent) {
    if (!child || !parent) return;
    child->uid = parent->uid;
    child->gid = parent->gid;
}

static inline int cred_is_root(const cred_t* c) {
    return c && c->uid == CRED_ROOT_UID;
}

// Minimum safe set_uid policy: only root may transition. Caller
// decides whose cred is written; this predicate only answers
// whether the caller's uid is allowed to change identity at all.
static inline int cred_transition_allowed(uint32_t caller_uid) {
    return caller_uid == CRED_ROOT_UID;
}

#endif // CRED_H
