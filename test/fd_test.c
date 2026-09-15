// Host-side unit test: P0 Phase 4 fd / open-description model (pure).
//
// Mirrors kernel/vfs_fd.c semantics WITHOUT SMP/locks/KyuzenFS:
//   - fd entries are per-task; open descriptions are heap objects
//   - dup/dup2 share the description (one offset, one buffer)
//   - close drops one reference; last close frees exactly once
//   - close_all releases only the exiting task's references
//   - invalid fds fail (-1), never crash / underflow / double-free
//
// The mock is single-threaded, but TEST 21 pins the SMP-relevant
// ordering rule deterministically: under the kernel's vfs_lock every
// dup-vs-close race serializes to either "dup wins then close" or
// "close wins then dup fails" — both must hold the refcount invariant.
//
// Build: clang -Iinclude test/fd_test.c -o test/fd_test
// Run:   ./test/fd_test  (or: make test-fd)

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "proc.h"   // PROC_* (stdio numbers, kill policy spot-check)
#include "cred.h"   // cred spot-check (P0.1 untouched)
#include "task.h"   // TASK_* constants only
#include "vfs.h"    // VFS_MAX_FDS, VFS_O_*, SYS_DUP/SYS_DUP2

#define MOCK_TASKS 4
#define MOCK_FDS   VFS_MAX_FDS
#define TTY 1
#define REG 0

typedef struct mock_open {
    int refs;          // reference count
    int kind;          // TTY / REG
    int freed;         // set on free (double-free tripwire)
    int size;          // valid bytes in data
    int pos;           // THE shared offset
    int flags;
    int dirty;
    char data[64];
} mock_open_t;

typedef struct {
    int used;
    int owner;
    mock_open_t* open;
} mock_fd_t;

static mock_fd_t  tab[MOCK_TASKS * MOCK_FDS];
static int cur;                 // "calling" task
static int allocs, frees;       // lifetime accounting (leak check)

static int slot_of(int task, int fd) {
    if (task < 0 || task >= MOCK_TASKS || fd < 0 || fd >= MOCK_FDS) return -1;
    return task * MOCK_FDS + fd;
}

static mock_open_t* mkopen(int kind, int flags) {
    static mock_open_t pool[64];
    static int next;
    assert(next < 64);
    mock_open_t* o = &pool[next++];
    o->refs = 1; o->kind = kind; o->freed = 0;
    o->size = 0; o->pos = 0; o->flags = flags; o->dirty = 0;
    allocs++;
    return o;
}

static void mget(mock_open_t* o) { assert(o && !o->freed); o->refs++; }

static void mput(mock_open_t* o) {
    assert(o && !o->freed && o->refs > 0);   // never underflow / double-release
    if (--o->refs == 0) { o->freed = 1; frees++; }
}

static mock_open_t* resolve(int fd) {
    int s = slot_of(cur, fd);
    if (s < 0 || !tab[s].used || tab[s].owner != cur || !tab[s].open) return NULL;
    return tab[s].open;
}

static int alloc_fd(void) {
    for (int i = 0; i < MOCK_FDS; i++)
        if (!tab[cur * MOCK_FDS + i].used) return i;
    return -1;
}

static void task_init(int t) {
    int save = cur; cur = t;
    for (int fd = 0; fd <= 2; fd++) {
        int s = slot_of(t, fd);
        if (tab[s].used) continue;
        tab[s].used = 1; tab[s].owner = t;
        tab[s].open = mkopen(TTY, fd == 0 ? VFS_O_RDONLY : VFS_O_WRONLY);
    }
    cur = save;
}

static int mopen(int flags) {   // regular file, empty, refcount 1
    int fd = alloc_fd();
    if (fd < 0) return -1;
    int s = slot_of(cur, fd);
    tab[s].used = 1; tab[s].owner = cur; tab[s].open = mkopen(REG, flags);
    return fd;
}

static int mdup(int oldfd) {
    int os = slot_of(cur, oldfd);
    if (os < 0 || !tab[os].used || tab[os].owner != cur || !tab[os].open) return -1;
    int nfd = alloc_fd();
    if (nfd < 0) return -1;
    int ns = slot_of(cur, nfd);
    mget(tab[os].open);
    tab[ns].used = 1; tab[ns].owner = cur; tab[ns].open = tab[os].open;
    return nfd;
}

static int mdup2(int oldfd, int newfd) {
    int os = slot_of(cur, oldfd), ns = slot_of(cur, newfd);
    if (os < 0 || ns < 0) return -1;
    if (!tab[os].used || tab[os].owner != cur || !tab[os].open) return -1;
    if (oldfd == newfd) return newfd;
    mock_open_t* src = tab[os].open;
    mget(src);   // acquire BEFORE releasing target (self-share safe)
    if (tab[ns].used && tab[ns].open) {
        mock_open_t* old = tab[ns].open;
        tab[ns].used = 0; tab[ns].open = NULL;
        mput(old);
    }
    tab[ns].used = 1; tab[ns].owner = cur; tab[ns].open = src;
    return newfd;
}

static int mclose(int fd) {
    int s = slot_of(cur, fd);
    if (s < 0 || !tab[s].used || tab[s].owner != cur || !tab[s].open) return -1;
    mock_open_t* o = tab[s].open;
    tab[s].used = 0; tab[s].open = NULL;
    mput(o);
    return 0;
}

static void close_all(int t) {
    for (int i = 0; i < MOCK_FDS; i++) {
        mock_fd_t* e = &tab[t * MOCK_FDS + i];
        if (e->used && e->open) {
            mock_open_t* o = e->open;
            e->used = 0; e->open = NULL;
            mput(o);
        }
    }
}

// Inherit helper: models the future-fork mechanism (child entries share
// the parent's descriptions, refs++). Spawn itself does NOT use this yet.
static void inherit(int child, int parent) {
    for (int i = 0; i < MOCK_FDS; i++) {
        mock_fd_t* p = &tab[parent * MOCK_FDS + i];
        mock_fd_t* c = &tab[child * MOCK_FDS + i];
        if (p->used && p->open && !c->used) {
            mget(p->open);
            c->used = 1; c->owner = child; c->open = p->open;
        }
    }
}

static int mwrite(int fd, const char* s, int n) {
    mock_open_t* o = resolve(fd);
    if (!o || !(o->flags & (VFS_O_WRONLY | VFS_O_RDWR))) return -1;
    if (o->kind == TTY) return (fd == 0) ? -1 : n;
    for (int i = 0; i < n && o->pos < 64; i++) o->data[o->pos++] = s[i];
    if (o->pos > o->size) o->size = o->pos;
    o->dirty = 1;
    return n;
}

static int mread(int fd, char* out, int n) {
    mock_open_t* o = resolve(fd);
    if (!o || (o->flags & VFS_O_WRONLY)) return -1;
    if (o->kind == TTY) return (fd != 0) ? -1 : 0;
    int avail = (o->pos < o->size) ? o->size - o->pos : 0;
    int k = (n < avail) ? n : avail;
    for (int i = 0; i < k; i++) out[i] = o->data[o->pos + i];
    o->pos += k;
    return k;
}

int main(void) {
    for (int t = 0; t < MOCK_TASKS; t++) task_init(t);
    cur = 0;

    // TEST 1: open creates fd. TEST 2: close releases it (slot reusable).
    int fd3 = mopen(VFS_O_RDWR);
    assert(fd3 == 3);                       // 0/1/2 are stdio
    assert(mclose(fd3) == 0);
    assert(mopen(VFS_O_RDWR) == 3);         // lowest free reused
    fd3 = 3;

    // TEST 3+4: dup works on regular files, shares the description.
    mwrite(fd3, "ABCDE", 5);
    mock_open_t* before = resolve(fd3);
    int frees_at_dup = frees;
    int fd4 = mdup(fd3);
    assert(fd4 == 4 && resolve(fd4) == before && before->refs == 2);

    // TEST 5: shared offset — reads advance one global position.
    {
        char b[4];
        mock_open_t* o = resolve(fd3);
        o->pos = 0;
        assert(mread(fd3, b, 2) == 2 && b[0] == 'A' && b[1] == 'B');
        assert(o->pos == 2);
        assert(mread(fd4, b, 2) == 2 && b[0] == 'C' && b[1] == 'D');
        assert(o->pos == 4);                // advanced globally, not per-fd
    }

    // TEST 6: closing original leaves duplicate valid.
    assert(mclose(fd3) == 0 && before->refs == 1 && !before->freed);
    {
        char b[2];
        mock_open_t* o = resolve(fd4);
        o->pos = 0;
        assert(mread(fd4, b, 1) == 1 && b[0] == 'A');
    }

    // TEST 7: closing duplicate finally releases the object (once).
    assert(mclose(fd4) == 0 && before->freed && frees == frees_at_dup + 1);

    // TEST 8: dup2 replaces target.
    int fa = mopen(VFS_O_RDWR);             // fd3 = A
    int fb = mopen(VFS_O_RDWR);             // fd4 = B
    mock_open_t *A = resolve(fa), *B = resolve(fb);
    assert(mdup2(fa, fb) == fb);
    assert(resolve(fb) == A && A->refs == 2);
    assert(B->freed);                       // displaced B released (no leak)

    // TEST 9: dup2 same-fd is a validated no-op.
    assert(mdup2(fa, fa) == fa && A->refs == 2);

    // TEST 10: dup2 self-share (both ends same) neither leaks nor frees.
    assert(mdup2(fa, fb) == fb && A->refs == 2 && !A->freed);
    assert(mclose(fa) == 0 && mclose(fb) == 0 && A->freed);

    // TEST 11+12: invalid fds rejected, no state touched.
    assert(mdup(-1) == -1 && mdup(99) == -1 && mdup(fa) == -1);
    assert(mclose(99) == -1 && mclose(-1) == -1);
    assert(mdup2(-1, 3) == -1 && mdup2(3, -1) == -1 && mdup2(3, 99) == -1);
    assert(mdup2(9, 10) == -1);             // closed oldfd

    // TEST 13: fd table isolation between tasks.
    cur = 0; int t0fd = mopen(VFS_O_RDWR);
    cur = 1;
    assert(resolve(t0fd) == NULL || slot_of(1, t0fd) != slot_of(0, t0fd));
    {
        int s = slot_of(1, t0fd);
        assert(!tab[s].used || tab[s].owner != 1);  // task1 sees nothing of task0
    }
    assert(mdup(t0fd) == -1 || resolve(mdup(t0fd)) != resolve(t0fd));
    cur = 0;
    assert(mclose(t0fd) == 0);

    // TEST 14: stdio uses the new descriptor model (TTY descriptions).
    for (int i = 0; i <= 2; i++) {
        mock_open_t* o = resolve(i);
        assert(o && o->kind == TTY && o->refs >= 1);
    }
    assert(resolve(0) != resolve(1));       // separate (RDONLY vs WRONLY)
    {
        int d = mdup(1);                    // stdout dup shares
        assert(d >= 0 && resolve(d) == resolve(1));
        assert(mclose(d) == 0);
    }

    // TEST 15+17: inherited references stay valid across one task's exit.
    cur = 0; int shared = mopen(VFS_O_RDWR);
    mock_open_t* so = resolve(shared);
    mwrite(shared, "XYZ", 3);
    inherit(1, 0);                          // future-fork mechanism
    cur = 1;
    assert(resolve(shared) == so && so->refs == 2);
    cur = 0; close_all(0);                  // "process A exits"
    assert(so->refs == 1 && !so->freed);
    cur = 1;                                // "process B" still valid
    {
        char b[4];
        so->pos = 0;
        assert(mread(shared, b, 3) == 3 && b[0] == 'X');
    }

    // TEST 16: process exit releases references correctly (last put frees).
    close_all(1);
    assert(so->freed);

    // TEST 18+19: no underflow, no double-free (failed closes don't put).
    assert(mclose(shared) == -1);           // already closed via close_all
    cur = 0;
    assert(mclose(shared) == -1);

    // TEST 21 (SMP ordering, deterministic): serialized dup-vs-close.
    {
        cur = 0;
        int f = mopen(VFS_O_RDWR);
        mock_open_t* o = resolve(f);
        // Order 1: dup wins, then close — description lives on via dup.
        int d = mdup(f);
        assert(mclose(f) == 0 && !o->freed && o->refs == 1);
        assert(mclose(d) == 0 && o->freed);
        // Order 2: close wins, then dup fails — no resurrection.
        f = mopen(VFS_O_RDWR);
        o = resolve(f);
        assert(mclose(f) == 0 && o->freed);
        assert(mdup(f) == -1);
    }

    // TEST 20: existing credential/proc/kill policy untouched.
    {
        cred_t u = { 1000, 1000 }, c = { 0, 0 };
        cred_inherit(&c, &u);
        assert(c.uid == 1000 && proc_can_kill(0, 3, 3, 5, 1, 1));
        assert(!proc_can_kill(0, 7, 3, 5, 1, 1));
        assert(PROC_STDIN == 0 && PROC_STDOUT == 1 && PROC_STDERR == 2);
        assert(SYS_DUP == 74 && SYS_DUP2 == 75);
    }

    // No leaks: every allocated description freed exactly once.
    close_all(0); close_all(1); close_all(2); close_all(3);
    assert(allocs == frees);

    printf("[fd] all 21 fd/open-description tests passed\n");
    return 0;
}
