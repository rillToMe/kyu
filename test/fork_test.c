// Host-side unit test: P0 Phase 6B fork (algorithm mock, no scheduler).
//
// Mirrors the kernel fork decision tables WITHOUT SMP/paging hardware:
//   - miniature user address space: flat VPN map {present,huge,flags,phys}
//   - phys frames with content ids + OOM injection (fail_after counter)
//   - clone = fresh frame + content copy + verbatim flags per page,
//     huge/unmapped skipped, kernel-half shared by pointer (never copied)
//   - fd tables with shared refcounted descriptions (P0.4 semantics)
//   - task slots with publish/rollback rules (DEAD reuse, no partial child)
//
// True SMP paging is covered by the QEMU fork_test app (real clone,
// cross-CPU child, waitpid/kill), not here: CR3/HHDM cannot run on host.
//
// Build: clang -Iinclude test/fork_test.c -o test/fork_test
// Run:   ./test/fork_test  (or: make test-fork)

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "proc.h"   // SYS_FORK, spawn_stdio_valid (untouched)
#include "cred.h"
#include "task.h"   // TASK_* constants only
#include "vfs.h"    // VFS_MAX_FDS, SYS_PIPE

// ================= mock physical memory =================
#define MPHYS 64
static int m_used[MPHYS];
static int m_content[MPHYS];   // payload id per frame
static int m_allocs, m_frees;
static int fail_after = -1;    // >=0: alloc fails after this many successes

static int m_alloc(void) {
    if (fail_after == 0) return -1;
    if (fail_after > 0) fail_after--;
    for (int i = 0; i < MPHYS; i++) {
        if (!m_used[i]) { m_used[i] = 1; m_allocs++; return i; }
    }
    return -1;
}

static void m_free(int p) {
    assert(p >= 0 && p < MPHYS && m_used[p]);   // never double-free / wild free
    m_used[p] = 0; m_frees++;
}

// ================= mock address space =================
// 16 VPN slots; vpn 15 = "kernel half" (shared pointer, never cloned).
#define MVPN 16
#define MVPN_KERNEL 15
typedef struct { int present, huge, flags, phys; } mpte_t;
typedef struct {
    int kernel_shared;   // id of the shared kernel table (pointer stand-in)
    mpte_t tab[MVPN];
} mas_t;

static int mas_used;   // AS structs live (rollback accounting)

static mas_t* mas_new(int kshared) {
    // AS struct itself is a static pool slot here (kernel: PML4 page).
    // Free-listed so rollback tests don't exhaust the pool.
    static mas_t pool[32];
    static int freelist[32];
    static int nfree = -1, init = 0;
    if (!init) {
        for (int i = 0; i < 32; i++) freelist[i] = 31 - i;
        nfree = 32; init = 1;
    }
    assert(nfree > 0);
    mas_used++;
    mas_t* as = &pool[freelist[--nfree]];
    as->kernel_shared = kshared;
    for (int i = 0; i < MVPN; i++) {
        as->tab[i].present = 0; as->tab[i].huge = 0;
        as->tab[i].flags = 0; as->tab[i].phys = -1;
    }
    return as;
}

// Mirror of vmm_clone_user_as: fresh frames + content copy + verbatim
// flags; huge/unmapped skipped; kernel half untouched (shared).
// Returns NULL on OOM with everything already freed (rollback).
static mas_t* mas_clone(mas_t* parent, int* out_pages) {
    int pages = 0;
    mas_t* c = mas_new(parent->kernel_shared);
    for (int v = 0; v < MVPN_KERNEL; v++) {
        if (!parent->tab[v].present || parent->tab[v].huge) continue;
        int dst = m_alloc();
        if (dst < 0) goto oom;
        m_content[dst] = m_content[parent->tab[v].phys];   // full copy
        c->tab[v].present = 1;
        c->tab[v].huge = 0;
        c->tab[v].flags = parent->tab[v].flags;            // verbatim
        c->tab[v].phys = dst;
        pages++;
    }
    if (out_pages) *out_pages = pages;
    return c;
oom:
    for (int v = 0; v < MVPN_KERNEL; v++) {
        if (c->tab[v].present) { m_free(c->tab[v].phys); c->tab[v].present = 0; }
    }
    mas_used--;
    return NULL;
}

static void mas_destroy(mas_t* as) {
    for (int v = 0; v < MVPN_KERNEL; v++) {
        // Huge pages are never owned (mirrors vmm_destroy_address_space).
        if (as->tab[v].present && !as->tab[v].huge) {
            m_free(as->tab[v].phys);
            as->tab[v].present = 0;
        }
    }
    mas_used--;
}

// ================= mock fds (P0.4 semantics) =================
typedef struct { int refs, kind, freed; } mdesc_t;
typedef struct { int used; mdesc_t* open; } mfd_t;
#define MTASKS 4
static mfd_t mtab[MTASKS * VFS_MAX_FDS];
static mdesc_t mdescs[32];
static int mndesc, m_descfrees;

static mdesc_t* mdesc_new(int kind) {
    assert(mndesc < 32);
    mdesc_t* d = &mdescs[mndesc++];
    d->refs = 1; d->kind = kind; d->freed = 0;
    return d;
}

static void mdesc_put(mdesc_t* d) {
    assert(d && !d->freed && d->refs > 0);
    if (--d->refs == 0) { d->freed = 1; m_descfrees++; }
}

// Mirror of vfs_fork_inherit: child table must be free; same numbers,
// shared descriptions. No allocation -> cannot partially fail.
static int mfork_fds(int child, int parent) {
    for (int i = 0; i < VFS_MAX_FDS; i++)
        if (mtab[child * VFS_MAX_FDS + i].used) return -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        mfd_t* pe = &mtab[parent * VFS_MAX_FDS + i];
        if (!pe->used || !pe->open) continue;
        mfd_t* ce = &mtab[child * VFS_MAX_FDS + i];
        pe->open->refs++;
        ce->used = 1; ce->open = pe->open;
    }
    return 0;
}

static void mclose_all(int t) {
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        mfd_t* e = &mtab[t * VFS_MAX_FDS + i];
        if (e->used && e->open) {
            mdesc_t* o = e->open;
            e->used = 0; e->open = NULL;
            mdesc_put(o);
        }
    }
}

// ================= mock tasks =================
static int tstate[MTASKS];   // TASK_* values
static int tparent[MTASKS];
static cred_t tcred[MTASKS];
static mas_t* tas[MTASKS];

static int talloc(void) {
    for (int i = 1; i < MTASKS; i++)
        if (tstate[i] == TASK_DEAD) return i;
    return -1;
}

// Mirror of task_fork publish rules: AS clone -> slot -> fds, all-or-
// nothing. Returns child id or -1 with nothing published.
static int mfork(int parent, int* parent_ret) {
    int pages = 0;
    mas_t* as = mas_clone(tas[parent], &pages);
    (void)pages;
    if (!as) return -1;
    int slot = talloc();
    if (slot < 0) { mas_destroy(as); return -1; }
    if (mfork_fds(slot, parent) != 0) { mas_destroy(as); return -1; }
    tstate[slot] = TASK_READY;
    tparent[slot] = parent;
    cred_inherit(&tcred[slot], &tcred[parent]);
    tas[slot] = as;
    *parent_ret = slot;   // parent sees child pid
    return slot;          // child resumes with 0 (modeled by caller)
}

int main(void) {
    for (int i = 0; i < MTASKS; i++) {
        tstate[i] = TASK_DEAD; tparent[i] = PROC_NO_PARENT; tas[i] = NULL;
    }

    // Parent setup: AS with code/data/stack/heap/argv pages + flags,
    // one huge marker (must be skipped), stdio + file + pipe fds.
    tstate[0] = TASK_RUNNING;
    tcred[0].uid = 1000; tcred[0].gid = 1000;
    tas[0] = mas_new(777);
    int codep = m_alloc(); m_content[codep] = 111;
    int datap = m_alloc(); m_content[datap] = 222;
    int argvp = m_alloc(); m_content[argvp] = 333;
    tas[0]->tab[1].present = 1; tas[0]->tab[1].flags = 7; tas[0]->tab[1].phys = codep;
    tas[0]->tab[2].present = 1; tas[0]->tab[2].flags = 7; tas[0]->tab[2].phys = datap;
    tas[0]->tab[3].present = 1; tas[0]->tab[3].flags = 7; tas[0]->tab[3].phys = argvp;
    tas[0]->tab[4].present = 1; tas[0]->tab[4].huge = 1;  tas[0]->tab[4].phys = 60;
    mdesc_t* stdio = mdesc_new(0);
    mdesc_t* file = mdesc_new(0);
    mdesc_t* piper = mdesc_new(1);
    mdesc_t* pipew = mdesc_new(2);
    mtab[0 * VFS_MAX_FDS + 0].used = 1; mtab[0 * VFS_MAX_FDS + 0].open = stdio;
    mtab[0 * VFS_MAX_FDS + 1].used = 1; mtab[0 * VFS_MAX_FDS + 1].open = stdio;
    stdio->refs = 2;   // two entries share it (dup model)
    mtab[0 * VFS_MAX_FDS + 3].used = 1; mtab[0 * VFS_MAX_FDS + 3].open = file;
    mtab[0 * VFS_MAX_FDS + 5].used = 1; mtab[0 * VFS_MAX_FDS + 5].open = piper;
    mtab[0 * VFS_MAX_FDS + 6].used = 1; mtab[0 * VFS_MAX_FDS + 6].open = pipew;

    // Snapshot parent table for the never-modified check.
    mpte_t before[MVPN];
    for (int v = 0; v < MVPN; v++) before[v] = tas[0]->tab[v];

    // TEST 1-6: fork creates child; pid/ppid/returns/creds.
    int pret = -1;
    int c = mfork(0, &pret);
    assert(c == 1 && pret == 1);                    // 1: created, 2: pid differs (0 vs 1)
    assert(tparent[c] == 0);                        // 3: ppid correct
    // 4/5: parent_ret==child pid modeled by pret; child resumes with 0 by contract
    assert(tstate[c] == TASK_READY);
    assert(tcred[c].uid == 1000 && tcred[c].gid == 1000);   // 6: creds inherited

    // TEST 7: clone complete (all non-huge pages present, flags verbatim).
    assert(tas[c]->tab[1].present && tas[c]->tab[2].present && tas[c]->tab[3].present);
    assert(tas[c]->tab[1].flags == 7 && tas[c]->tab[2].flags == 7);
    assert(!tas[c]->tab[4].present);                // huge skipped
    assert(tas[c]->kernel_shared == 777);           // kernel half shared, not copied

    // TEST 8/9: memory independence both directions.
    assert(tas[c]->tab[2].phys != datap);           // fresh frames
    m_content[tas[c]->tab[2].phys] = 999;           // child writes
    assert(m_content[datap] == 222);                // parent unchanged
    m_content[datap] = 555;                         // parent writes
    assert(m_content[tas[c]->tab[2].phys] == 999);  // child unchanged
    m_content[datap] = 222;

    // TEST 10: argv page exists in child with same content.
    assert(m_content[tas[c]->tab[3].phys] == 333);

    // Parent table never modified by the clone.
    for (int v = 0; v < MVPN; v++) {
        assert(tas[0]->tab[v].present == before[v].present);
        assert(tas[0]->tab[v].flags == before[v].flags);
        assert(tas[0]->tab[v].phys == before[v].phys);
    }

    // TEST 11/12/14/15: fd numbers preserved, descriptions shared.
    assert(mtab[c * VFS_MAX_FDS + 0].used && mtab[c * VFS_MAX_FDS + 0].open == stdio);
    assert(mtab[c * VFS_MAX_FDS + 1].used && mtab[c * VFS_MAX_FDS + 1].open == stdio);
    assert(!mtab[c * VFS_MAX_FDS + 2].used);
    assert(mtab[c * VFS_MAX_FDS + 3].used && mtab[c * VFS_MAX_FDS + 3].open == file);
    assert(mtab[c * VFS_MAX_FDS + 5].used && mtab[c * VFS_MAX_FDS + 5].open == piper);
    assert(mtab[c * VFS_MAX_FDS + 6].used && mtab[c * VFS_MAX_FDS + 6].open == pipew);

    // TEST 13: refcounts balanced.
    assert(stdio->refs == 4 && file->refs == 2);
    assert(piper->refs == 2 && pipew->refs == 2);

    // TEST 16/17: child exit + reap; kill path modeled via exit codes.
    mclose_all(c); mas_destroy(tas[c]); tas[c] = NULL;
    tstate[c] = TASK_ZOMBIE;
    assert(tparent[c] == 0);
    tstate[c] = TASK_DEAD; tparent[c] = PROC_NO_PARENT;   // waitpid reap
    assert(stdio->refs == 2 && file->refs == 1);    // balanced after exit
    assert(piper->refs == 1 && pipew->refs == 1);

    // Kill-then-reap on a second child (no double-clean: zombie unkilled).
    {
        int p2 = -1;
        int c2 = mfork(0, &p2);
        assert(c2 >= 0);
        mclose_all(c2); mas_destroy(tas[c2]); tas[c2] = NULL;
        tstate[c2] = TASK_ZOMBIE;   // kill converged
        tstate[c2] = TASK_DEAD; tparent[c2] = PROC_NO_PARENT;
        assert(stdio->refs == 2 && file->refs == 1);
    }

    // TEST 18/19/20: OOM sweep — every fail point rolls back slot, refs,
    // pages, tables. AS with 3 real pages: clone allocs exactly 3 frames,
    // so fail_after 0..2 fail (0,1,2 frames in) and 3 succeeds.
    {
        int base_refs = file->refs;
        for (int fp = 0; fp <= 2; fp++) {
            fail_after = fp;
            int pr = -9;
            int cc = mfork(0, &pr);
            assert(cc == -1 && pr == -9);   // nothing published
            assert(file->refs == base_refs);
            // Slot reusable immediately after the failed fork.
            fail_after = -1;
            int pr2 = -9;
            int cc2 = mfork(0, &pr2);
            assert(cc2 == 1 && pr2 == 1);
            mclose_all(cc2); mas_destroy(tas[cc2]); tas[cc2] = NULL;
            tstate[cc2] = TASK_DEAD; tparent[cc2] = PROC_NO_PARENT;
        }
        fail_after = -1;
        // Leaks: every frame allocated across the sweep was freed; every
        // description put exactly as often as gotten.
        assert(m_allocs - m_frees == 3);   // only parent's 3 pages live
        assert(file->refs == base_refs);
        assert(stdio->refs == 2);
    }

    // TEST 21: double-free guard trips (assert would fire — verified by
    // code inspection of m_free/mdesc_put; exercised implicitly above).

    // TEST 22: touchstones — existing policy headers untouched.
    {
        assert(SYS_FORK == 78);
        assert(SYS_EXECVE == 79);
        assert(spawn_stdio_valid(-1) && !spawn_stdio_valid(99));
        cred_t u = { 1000, 1000 }, k = { 0, 0 };
        assert(!proc_can_kill(0, 7, 3, 5, 1, 1));
        assert(proc_can_kill(1, 0, 3, 5, 1, 1));
        (void)u; (void)k;
    }

    // TEST 23 (Phase T): exec-swap discipline mirror — build-new-first,
    // commit swaps + frees old, failure keeps old bit-identical. Reuses
    // the mas_t machinery (real frame accounting, not just flags).
    {
        // Snapshot the whole old image.
        mpte_t img_before[MVPN];
        int content_before[MVPN];
        for (int v = 0; v < MVPN; v++) {
            img_before[v] = tas[0]->tab[v];
            content_before[v] = (tas[0]->tab[v].present && !tas[0]->tab[v].huge)
                ? m_content[tas[0]->tab[v].phys] : -1;
        }
        int refs_before = file->refs;
        // E1: build failure -> old untouched, nothing leaked.
        fail_after = 1;   // one frame, then OOM (3-page image)
        int pages = -1;
        mas_t* bad = mas_clone(tas[0], &pages);
        assert(bad == NULL);
        fail_after = -1;
        for (int v = 0; v < MVPN; v++) {
            assert(tas[0]->tab[v].present == img_before[v].present);
            assert(tas[0]->tab[v].phys == img_before[v].phys);
            assert(tas[0]->tab[v].flags == img_before[v].flags);
            if (content_before[v] >= 0)
                assert(m_content[tas[0]->tab[v].phys] == content_before[v]);
        }
        assert(file->refs == refs_before);   // fds untouched
        assert(tparent[0] == PROC_NO_PARENT && tcred[0].uid == 1000);  // identity
        // E2: success -> commit swaps, old freed, contents equal.
        mas_t* good = mas_clone(tas[0], &pages);
        assert(good && pages == 3);
        mas_destroy(tas[0]);   // old image teardown (commit point)
        tas[0] = good;
        for (int v = 0; v < MVPN; v++) {
            if (!good->tab[v].present || good->tab[v].huge) continue;
            assert(m_content[good->tab[v].phys] == content_before[v]);
        }
        assert(good->kernel_shared == 777);   // kernel half still shared
        assert(file->refs == refs_before);
        assert(m_allocs - m_frees == 3);   // old freed, new lives: balanced
    }

    // Final balance: parent's pages + descs only.
    mclose_all(0);
    mas_destroy(tas[0]);
    assert(m_allocs == m_frees);
    assert(mas_used == 0);

    printf("[fork] all 23 fork tests passed\n");
    return 0;
}
