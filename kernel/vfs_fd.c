// kernel/vfs_fd.c — Per-task file descriptor layer over KyuzenFS.
//
// P0 Phase 4 model: two distinct concepts.
//
//   FD ENTRY (per task, static pool slot):
//     used / owner / fd_flags / open*
//     Belongs to exactly one task. Multiple entries may point at the
//     same open description.
//
//   OPEN DESCRIPTION (heap object, reference counted):
//     refcount / kind / path / buf+size+cap / pos / flags / dirty.
//     Owns the buffer, the file offset, and the flush-on-last-close
//     responsibility. Freed only when refcount reaches zero.
//
// Locking: one global vfs_lock guards the fd table, every refcount
// op, and the whole buffered-file data path (read/write/lseek buffer
// access + flush). Lifetime guarantee comes from that lock — no
// per-object lock, no new global lock. Only TTY device I/O drops
// vfs_lock (tty_read/write take the keyboard lock and must not nest
// under it); the TTY path holds an extra open reference across the
// drop so a concurrent close cannot free the description mid-I/O.
//
// Spawn inherits nothing by default: a new task gets three fresh stdio
// descriptions (fd 0/1/2 -> console TTY). Explicit inheritance for shell
// redirection/pipelines goes through vfs_inherit_stdio() (sys_spawn_redir):
// only the requested 0/1/2 entries are shared, everything else stays
// with the opener. Cross-task sharing beyond that is future fork() work;
// the refcount + vfs_lock mechanism already supports it (close_all releases
// only the exiting task's references).
//
// P0 Phase 5 pipes: VFS_KIND_PIPE_READ/WRITE descriptions share one
// vfs_pipe_t (bounded circular buffer + one wait queue whose lock guards
// ALL pipe state). Lock order is always vfs_lock -> pipe wq->lock, never
// the reverse. Pipe I/O holds a temp description reference across the
// vfs_lock drop (same TTY pattern), so close cannot free mid-I/O; kill of
// a pipe-blocked task reports via wait_block_killable so the temp ref is
// released before converging on proc_exit_kill (no leak).

#include "vfs.h"
#include "task.h"
#include "smp.h"
#include "spinlock.h"
#include "wait.h"
#include "vnode.h"
#include "kyuzenfs_v4.h"
#include <stddef.h>

extern void* kmalloc(uint32_t size);
extern void  kfree(void* ptr);
struct vnode* kfs_v4_root_vnode(void);
void kfs_sync_all(void);

// Open-description kinds. TTY descriptions carry no offset; file
// descriptions carry a VNODE (KyuzenFS V4 streaming, in-place write);
// pipe endpoint descriptions share one vfs_pipe_t (never a generic
// read/write fd: each description knows its direction).
#define VFS_KIND_TTY        1
#define VFS_KIND_FILE       0
#define VFS_KIND_PIPE_READ  2
#define VFS_KIND_PIPE_WRITE 3

// KyuzenFS V4: file description memegang VNODE (streaming per-block via
// bcache, write in-place) — TIDAK ada lagi buffer whole-file di RAM.
#include "vnode.h"
#include "kyuzenfs_v4.h"

// Pipe object: bounded circular byte buffer shared by exactly one read
// endpoint description and one write endpoint description (dups alias
// those descriptions). readers/writers are SIDE-alive flags (1 while
// the respective endpoint description exists), not fd counts: dup() of
// a read fd bumps the description refcount but leaves readers == 1.
// Freed only when both sides are gone AND no waiter can reference it —
// every pipe access path pins its description, so destruction implies
// no waiter is parked inside.
typedef struct vfs_pipe {
    wait_queue_t wq;       // guards everything below (monitor pattern)
    uint8_t*     buf;      // VFS_PIPE_CAP bytes, circular
    uint32_t     rpos;     // read index
    uint32_t     wpos;     // write index
    uint32_t     used;     // bytes available to readers
    uint32_t     readers;  // read endpoint description alive
    uint32_t     writers;  // write endpoint description alive
} vfs_pipe_t;

typedef struct vfs_open_file {
    volatile uint32_t refcount;   // references from fd entries (+ temp I/O refs)
    uint8_t  kind;                // VFS_KIND_* (pipe ends know their direction)
    char     path[VFS_MAX_PATH];
    // FILE only: vnode streaming (KyuzenFS V4) — offset/ukuran hidup di
    // vnode/inode; dirty + flush-on-last-close digantikan oleh sync berkala
    // (kfs_sync_all) dan vnode sync saat release.
    struct vnode* vnode;          // FILE only; NULL untuk TTY dan pipes
    uint32_t pos;                 // THE shared file offset (one per description)
    uint32_t flags;               // open-time status flags (VFS_O_*)
    vfs_pipe_t* pipe;             // pipe ends only; NULL otherwise
} vfs_open_file_t;

typedef struct {
    int              used;
    int              owner;       // task id
    uint32_t         fd_flags;    // descriptor-local flags (reserved; always 0 —
                                  // close-on-exec style bits belong here, not
                                  // in the open description)
    vfs_open_file_t* open;        // shared open description (never NULL if used)
} vfs_fd_entry_t;

static vfs_fd_entry_t fds[MAX_TASKS * VFS_MAX_FDS];  // pool; index = task_id*VFS_MAX_FDS + fd
static spinlock_t vfs_lock = SPINLOCK_INIT;

// Console TTY (drivers/tty.c). Declared here (not fs.h) to keep the fd layer
// independent of device init order; tty_node.funcs are set by init_tty().
extern uint32_t tty_write(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
extern uint32_t tty_read(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
extern fs_node_t tty_node;

void vfs_init(void) {
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    for (int i = 0; i < (int)(sizeof(fds) / sizeof(fds[0])); i++) {
        fds[i].used = 0;
        fds[i].open = NULL;
        fds[i].fd_flags = 0;
    }
    spinlock_unlock_irqrestore(&vfs_lock, f);
}

static void path_copy(char* dst, const char* src) {
    int i = 0;
    while (i < VFS_MAX_PATH - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Map a per-task fd number to a pool slot. Each task owns a contiguous window
// [task_id*VFS_MAX_FDS, +VFS_MAX_FDS). Caller holds vfs_lock.
static int slot_of(int task_id, int fd) {
    if (task_id < 0 || task_id >= MAX_TASKS) return -1;
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    return task_id * VFS_MAX_FDS + fd;
}

// Resolve the open description for fd in the CALLING task.
// Caller holds vfs_lock. Returns NULL if invalid/unowned.
static vfs_open_file_t* resolve_open(int fd) {
    int task_id = smp_current_task_id();
    int s = slot_of(task_id, fd);
    if (s < 0) return NULL;
    if (!fds[s].used || fds[s].owner != task_id || !fds[s].open) return NULL;
    return fds[s].open;
}

// Lowest free fd number in a task window. Caller holds vfs_lock.
static int alloc_fd(int task_id) {
    int base = task_id * VFS_MAX_FDS;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fds[base + i].used) return i;
    }
    return -1;
}

// Increment before publishing a new reference. Caller holds vfs_lock
// (which serializes all table/refcount mutation across CPUs).
static void open_get(vfs_open_file_t* of) {
    __sync_fetch_and_add(&of->refcount, 1);
}

// KyuzenFS V4: tidak ada flush_locked delete+recreate — write in-place
// langsung ke block fisik via vnode; metadata disinkronkan oleh
// kfs_sync_all() berkala dan vnode sync saat release.

// A pipe endpoint closed: drop its side flag and wake everyone on the
// other side (readers get EOF when writers hit 0; writers fail when
// readers hit 0). Frees the pipe only on the (0,0) transition —
// serialized here, so exactly one closer frees. kfree runs OUTSIDE the
// wq lock (heap lock must not nest under it). Caller holds vfs_lock
// (order vfs_lock -> wq->lock); waiters are impossible here because
// every waiter pins its endpoint description, and this runs only when
// an endpoint description dies.
static void pipe_end_closed(vfs_pipe_t* p, uint8_t kind) {
    uint64_t f = wait_queue_lock(&p->wq);
    if (kind == VFS_KIND_PIPE_READ) p->readers = 0;
    else p->writers = 0;
    wait_wake_all(&p->wq);
    int dead = (p->readers == 0 && p->writers == 0);
    wait_queue_unlock(&p->wq, f);
    if (dead) {
        if (p->buf) kfree(p->buf);
        kfree(p);
    }
}

// Release one reference. Frees only when the count reaches zero — never
// underflows (callers always own the reference they drop), never
// double-releases (each fd entry holds exactly one reference, cleared
// when the entry is removed). Caller holds vfs_lock.
static void open_put(vfs_open_file_t* of) {
    uint32_t left = __sync_sub_and_fetch(&of->refcount, 1);
    if (left != 0) return;
    if (of->kind == VFS_KIND_FILE) {
        if (of->vnode) {
            of->vnode->ops->sync(of->vnode);       // metadata + bcache flush
            of->vnode->ops->release(of->vnode);    // refcount terakhir di sini
            of->vnode = NULL;
        }
    } else if (of->kind == VFS_KIND_PIPE_READ || of->kind == VFS_KIND_PIPE_WRITE) {
        if (of->pipe) pipe_end_closed(of->pipe, of->kind);
    }
    kfree(of);
}

// Allocate an open description with refcount 1. Vnode (FILE) dimiliki
// caller pada sukses (berpindah ke description); dilepas di sini saat gagal.
static vfs_open_file_t* open_alloc(uint8_t kind, const char* path,
                                   struct vnode* vnode, uint32_t pos,
                                   uint32_t flags) {
    vfs_open_file_t* of = (vfs_open_file_t*)kmalloc(sizeof(vfs_open_file_t));
    if (!of) return NULL;
    of->refcount = 1;
    of->kind  = kind;
    path_copy(of->path, path ? path : "");
    of->vnode = vnode;
    of->pos   = pos;
    of->flags = flags;
    of->pipe  = NULL;
    return of;
}

// Install one fd in a task window pointing at an existing description
// (takes one more reference). Caller holds vfs_lock; slot must be free.
static void install_locked(int task_id, int fd, vfs_open_file_t* of) {
    int s = slot_of(task_id, fd);
    if (s < 0 || fds[s].used) return;
    open_get(of);
    fds[s].used     = 1;
    fds[s].owner    = task_id;
    fds[s].fd_flags = 0;
    fds[s].open     = of;
}

// Install one fresh TTY description. Caller holds vfs_lock.
static void install_tty_locked(int task_id, int fd, uint32_t flags) {
    int s = slot_of(task_id, fd);
    if (s < 0) return;
    if (fds[s].used) return;   // idempotent: never clobber a live fd
    vfs_open_file_t* of = open_alloc(VFS_KIND_TTY, "tty", NULL, 0, flags);
    if (!of) return;
    fds[s].used     = 1;
    fds[s].owner    = task_id;
    fds[s].fd_flags = 0;
    fds[s].open     = of;   // refcount 1 moves into the entry, no extra get
}

void vfs_task_init(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    // Three SEPARATE descriptions (not one shared): stdin is O_RDONLY
    // while stdout/stderr are O_WRONLY, and permission checks read the
    // description flags — sharing one description would merge them.
    // All three route to the same console device.
    install_tty_locked(task_id, 0, VFS_O_RDONLY);   // stdin
    install_tty_locked(task_id, 1, VFS_O_WRONLY);   // stdout
    install_tty_locked(task_id, 2, VFS_O_WRONLY);   // stderr
    spinlock_unlock_irqrestore(&vfs_lock, f);
}

int vfs_open(const char* path, uint32_t flags) {
    if (path == NULL || path[0] == '\0') return -1;

    int task_id = smp_current_task_id();
    if (task_id < 0 || task_id >= MAX_TASKS) return -1;

    // KyuzenFS V4: resolve via root vnode (lookup/create/truncate), lalu
    // description hanya menyimpan vnode + offset — streaming per-block.
    struct vnode* root = kfs_v4_root_vnode();
    if (!root) return -1;

    struct vnode* vn = NULL;
    if (root->ops->lookup(root, path, &vn) != KZFS_EOK || !vn) {
        if (!(flags & VFS_O_CREAT)) { root->ops->release(root); return -1; }
        if (root->ops->create(root, path, 0, &vn) != KZFS_EOK || !vn) {
            root->ops->release(root);
            return -1;
        }
    } else if (flags & VFS_O_TRUNC) {
        vn->ops->truncate(vn, 0);
    }
    root->ops->release(root);

    uint64_t fsize = vn->size;
    if (vn->ops->open(vn, (int)flags) != KZFS_EOK) {
        vn->ops->release(vn);
        return -1;
    }

    vfs_open_file_t* of = open_alloc(VFS_KIND_FILE, path, vn,
                                     (flags & VFS_O_APPEND) ? (uint32_t)fsize : 0,
                                     flags);
    if (!of) { vn->ops->release(vn); return -1; }   // vnode ref pindah ke of

    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    int fd = alloc_fd(task_id);
    if (fd < 0) {
        spinlock_unlock_irqrestore(&vfs_lock, f);
        vn->ops->release(vn);      // lepas ref vnode
        kfree(of);
        return -1;   // fd table full
    }
    int s = slot_of(task_id, fd);
    fds[s].used     = 1;
    fds[s].owner    = task_id;
    fds[s].fd_flags = 0;
    fds[s].open     = of;   // refcount 1 moves into the entry
    spinlock_unlock_irqrestore(&vfs_lock, f);
    return fd;
}

// PIPE_KILLED: pipe_read/pipe_write observed kill_pending while blocked.
// The caller drops its temp description reference, then converges on the
// authoritative exit itself (wait_block_killable already dequeued us).
#define PIPE_KILLED (-2)

// Read from a pipe endpoint. No locks held on entry; the caller pins the
// endpoint description (temp ref). Monitor pattern on pipe->wq: re-check
// the condition after every wakeup. Empty + writers alive -> block (no
// busy-wait); empty + no writers -> EOF (0). Wakes writers when draining.
static int pipe_read(vfs_open_file_t* of, uint8_t* out, uint32_t count) {
    vfs_pipe_t* p = of->pipe;
    if (!p) return -1;
    uint64_t f = wait_queue_lock(&p->wq);
    for (;;) {
        if (p->used > 0) break;
        if (p->writers == 0) { wait_queue_unlock(&p->wq, f); return 0; }
        int killed = 0;
        f = wait_block_killable(&p->wq, f, &killed);
        if (killed) { wait_queue_unlock(&p->wq, f); return PIPE_KILLED; }
    }
    uint32_t n = (count < p->used) ? count : p->used;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = p->buf[p->rpos];
        p->rpos = (p->rpos + 1) % VFS_PIPE_CAP;
    }
    p->used -= n;
    wait_wake_all(&p->wq);   // space freed: blocked writers may proceed
    wait_queue_unlock(&p->wq, f);
    return (int)n;
}

// Write to a pipe endpoint. Partial writes allowed: copies min(count, free)
// and returns the byte count, so a writer always makes progress when any
// space exists and blocks only on a completely full pipe. No readers ->
// -1 (KyuzenOS has no SIGPIPE; the writer fails, it is never killed).
static int pipe_write(vfs_open_file_t* of, const uint8_t* in, uint32_t count) {
    vfs_pipe_t* p = of->pipe;
    if (!p) return -1;
    uint64_t f = wait_queue_lock(&p->wq);
    for (;;) {
        if (p->readers == 0) { wait_queue_unlock(&p->wq, f); return -1; }
        if (p->used < VFS_PIPE_CAP) break;
        int killed = 0;
        f = wait_block_killable(&p->wq, f, &killed);
        if (killed) { wait_queue_unlock(&p->wq, f); return PIPE_KILLED; }
    }
    uint32_t free = VFS_PIPE_CAP - p->used;
    uint32_t n = (count < free) ? count : free;
    for (uint32_t i = 0; i < n; i++) {
        p->buf[p->wpos] = in[i];
        p->wpos = (p->wpos + 1) % VFS_PIPE_CAP;
    }
    p->used += n;
    wait_wake_all(&p->wq);   // data arrived: blocked readers may proceed
    wait_queue_unlock(&p->wq, f);
    return (int)n;
}

// Drop a temp pipe/TTY reference and converge on the kill exit. The waiter
// is already dequeued; close_all in the exit path releases our fd entries.
// Noreturn.
static void pipe_io_killed(vfs_open_file_t* of) {
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    open_put(of);
    spinlock_unlock_irqrestore(&vfs_lock, f);
    proc_exit_kill();
}

int vfs_read(int fd, void* buf, uint32_t count) {
    if (buf == NULL) return -1;
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    vfs_open_file_t* of = resolve_open(fd);
    // Implicit stdio: fd 0/1/2 behave as TTY even if vfs_task_init never ran
    // (task 0 / early tasks). Never auto-install here; just service directly.
    if (!of && fd >= 0 && fd <= 2) {
        int tid = smp_current_task_id();
        int s = slot_of(tid, fd);
        if (s >= 0 && !fds[s].used) {
            spinlock_unlock_irqrestore(&vfs_lock, f);
            if (fd != 0) return -1;   // stdout/stderr are write-only
            if (count == 0) return 0;
            if (!tty_node.read) return -1;
            return (int)tty_read(&tty_node, 0, count, (uint8_t*)buf);
        }
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return -1;
    }
    if (!of || (of->flags & VFS_O_WRONLY)) {
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return -1;
    }
    if (of->kind == VFS_KIND_PIPE_READ) {
        // Write ends never reach here (rejected by the WRONLY check
        // above: every write description carries VFS_O_WRONLY).
        if (count == 0) { spinlock_unlock_irqrestore(&vfs_lock, f); return 0; }
        open_get(of);   // pin across the vfs_lock drop (may block inside)
        spinlock_unlock_irqrestore(&vfs_lock, f);
        int n = pipe_read(of, (uint8_t*)buf, count);
        if (n == PIPE_KILLED) pipe_io_killed(of);   // noreturn
        f = spinlock_lock_irqsave(&vfs_lock);
        open_put(of);
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return n;
    }
    if (of->kind == VFS_KIND_TTY) {
        // stdin: only fd 0 is readable; stdout/stderr reject reads.
        if (fd != 0) { spinlock_unlock_irqrestore(&vfs_lock, f); return -1; }
        // Hold a temp reference across the lock drop so a concurrent
        // close on another CPU cannot free the description mid-read.
        // (TTY carries no offset/buffer, so no state is lost.)
        open_get(of);
        spinlock_unlock_irqrestore(&vfs_lock, f);
        int n = -1;
        if (count != 0 && tty_node.read)
            n = (int)tty_read(&tty_node, 0, count, (uint8_t*)buf);
        else if (count == 0)
            n = 0;
        f = spinlock_lock_irqsave(&vfs_lock);
        open_put(of);
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return n;
    }
    // KyuzenFS V4: streaming read via vnode (block cache); offset SHARED
    // satu per description (alias dup/fork melihat offset yang sama).
    if (count > (uint32_t)0x40000000u) count = 0x40000000u;  // clamp int-safe
    uint64_t got = 0;
    int rc = of->vnode->ops->read(of->vnode, of->pos, buf, count, &got);
    of->pos += (uint32_t)got;
    spinlock_unlock_irqrestore(&vfs_lock, f);
    if (rc != KZFS_EOK && got == 0) return -1;
    return (int)got;
}

int vfs_write(int fd, const void* buf, uint32_t count) {
    if (buf == NULL) return -1;
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    vfs_open_file_t* of = resolve_open(fd);
    // Implicit stdio fallback (see vfs_read): stdout/stderr write to TTY.
    if (!of && fd >= 0 && fd <= 2) {
        int tid = smp_current_task_id();
        int s = slot_of(tid, fd);
        if (s >= 0 && !fds[s].used) {
            spinlock_unlock_irqrestore(&vfs_lock, f);
            if (fd == 0) return -1;   // stdin is read-only
            if (count == 0) return 0;
            if (!tty_node.write) return -1;
            return (int)tty_write(&tty_node, 0, count, (uint8_t*)buf);
        }
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return -1;
    }
    if (!of || !(of->flags & (VFS_O_WRONLY | VFS_O_RDWR))) {
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return -1;
    }
    if (of->kind == VFS_KIND_PIPE_WRITE) {
        // Read ends never reach here (rejected by the flags check above:
        // every read description is VFS_O_RDONLY).
        if (count == 0) { spinlock_unlock_irqrestore(&vfs_lock, f); return 0; }
        open_get(of);   // pin across the vfs_lock drop (may block inside)
        spinlock_unlock_irqrestore(&vfs_lock, f);
        int n = pipe_write(of, (const uint8_t*)buf, count);
        if (n == PIPE_KILLED) pipe_io_killed(of);   // noreturn
        f = spinlock_lock_irqsave(&vfs_lock);
        open_put(of);
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return n;
    }
    if (of->kind == VFS_KIND_TTY) {
        if (fd == 0) { spinlock_unlock_irqrestore(&vfs_lock, f); return -1; }
        open_get(of);   // temp ref across the lock drop (see vfs_read)
        spinlock_unlock_irqrestore(&vfs_lock, f);
        int n = -1;
        if (count != 0 && tty_node.write)
            n = (int)tty_write(&tty_node, 0, count, (uint8_t*)buf);
        else if (count == 0)
            n = 0;
        f = spinlock_lock_irqsave(&vfs_lock);
        open_put(of);
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return n;
    }
    // KyuzenFS V4: streaming write IN-PLACE via vnode — block fisik yang
    // sudah ada langsung ditulisi; block baru dialokasi hanya bila file
    // membesar. O_APPEND menulis selalu di ujung file (vnode->size live).
    if (of->flags & VFS_O_APPEND) of->pos = (uint32_t)of->vnode->size;
    if (count > (uint32_t)0x40000000u) count = 0x40000000u;  // clamp int-safe
    uint64_t wrote = 0;
    int rc = of->vnode->ops->write(of->vnode, of->pos, buf, count, &wrote);
    of->pos += (uint32_t)wrote;
    spinlock_unlock_irqrestore(&vfs_lock, f);
    if (rc != KZFS_EOK && wrote == 0) return -1;
    return (int)wrote;
}

int vfs_lseek(int fd, int32_t offset, int whence) {
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    vfs_open_file_t* of = resolve_open(fd);
    if (!of) { spinlock_unlock_irqrestore(&vfs_lock, f); return -1; }
    if (of->kind != VFS_KIND_FILE) { spinlock_unlock_irqrestore(&vfs_lock, f); return -1; }

    int64_t base = (whence == VFS_SEEK_CUR) ? (int64_t)of->pos
                 : (whence == VFS_SEEK_END) ? (int64_t)of->vnode->size
                 : 0;
    int64_t np = base + offset;
    if (np < 0 || np > UINT32_MAX) {   // pos is uint32_t; reject silent truncation (bug 2.2)
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return -1;
    }
    of->pos = (uint32_t)np;
    spinlock_unlock_irqrestore(&vfs_lock, f);
    return (int)of->pos;
}

// Create a pipe: two endpoint descriptions on one shared pipe object.
// Both-or-neither under a single vfs_lock hold: the two lowest free fds
// are located first, and anything allocated is released if the table
// cannot fit both ends. Returns 0 with fds[0]=read, fds[1]=write, or -1.
int vfs_pipe(int out[2]) {
    if (!out) return -1;
    int task_id = smp_current_task_id();
    if (task_id < 0 || task_id >= MAX_TASKS) return -1;

    vfs_pipe_t* p = (vfs_pipe_t*)kmalloc(sizeof(vfs_pipe_t));
    uint8_t* buf = (uint8_t*)kmalloc(VFS_PIPE_CAP);
    vfs_open_file_t *ro = NULL, *wo = NULL;
    if (p && buf) {
        wait_queue_init(&p->wq);
        p->buf = buf; p->rpos = 0; p->wpos = 0; p->used = 0;
        p->readers = 1; p->writers = 1;
        ro = open_alloc(VFS_KIND_PIPE_READ, "pipe", NULL, 0, VFS_O_RDONLY);
        if (ro) wo = open_alloc(VFS_KIND_PIPE_WRITE, "pipe", NULL, 0, VFS_O_WRONLY);
    }
    if (!p || !buf || !ro || !wo) {
        if (wo) kfree(wo);
        if (ro) kfree(ro);
        if (buf) kfree(buf);
        if (p) kfree(p);
        return -1;
    }
    ro->pipe = p;
    wo->pipe = p;

    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    int base = task_id * VFS_MAX_FDS;
    int rfd = -1, wfd = -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fds[base + i].used) {
            if (rfd < 0) rfd = i;
            else { wfd = i; break; }
        }
    }
    if (rfd < 0 || wfd < 0) {
        spinlock_unlock_irqrestore(&vfs_lock, f);
        kfree(wo);
        kfree(ro);
        kfree(buf);
        kfree(p);
        return -1;   // fd table full: no half-created pipe
    }
    fds[base + rfd].used = 1; fds[base + rfd].owner = task_id;
    fds[base + rfd].fd_flags = 0; fds[base + rfd].open = ro;   // refcount 1 moves in
    fds[base + wfd].used = 1; fds[base + wfd].owner = task_id;
    fds[base + wfd].fd_flags = 0; fds[base + wfd].open = wo;
    spinlock_unlock_irqrestore(&vfs_lock, f);
    out[0] = rfd;
    out[1] = wfd;
    return 0;
}

// Explicit stdio inheritance for spawn (sys_spawn_redir). The child must
// not be runnable yet (called between slot assignment and runq_push).
// spec[i] < 0 keeps the child's fresh TTY entry; otherwise the child's
// fd i shares the parent's open description (refcount++), replacing the
// fresh TTY (released). Only fd 0/1/2 are inheritable — never the whole
// table, never across arbitrary tasks. Returns 0 or -1; the caller
// releases a half-inherited child with vfs_close_all().
int vfs_inherit_stdio(int child, int parent, const int spec[3]) {
    if (!spec) return -1;
    if (child < 0 || child >= MAX_TASKS || parent < 0 || parent >= MAX_TASKS) return -1;
    if (child == parent) return -1;
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    for (int i = 0; i < 3; i++) {
        if (spec[i] < 0) continue;
        int ps = slot_of(parent, spec[i]);
        if (ps < 0 || !fds[ps].used || fds[ps].owner != parent || !fds[ps].open) {
            spinlock_unlock_irqrestore(&vfs_lock, f);
            return -1;
        }
        int cs = slot_of(child, i);
        if (cs < 0 || !fds[cs].used || fds[cs].owner != child || !fds[cs].open) {
            spinlock_unlock_irqrestore(&vfs_lock, f);
            return -1;
        }
        vfs_open_file_t* src = fds[ps].open;
        open_get(src);
        vfs_open_file_t* old = fds[cs].open;
        fds[cs].open = src;
        fds[cs].fd_flags = 0;
        open_put(old);   // fresh TTY, last ref: freed here
    }
    spinlock_unlock_irqrestore(&vfs_lock, f);
    return 0;
}

// Whole-table inheritance for fork() (P0 Phase 6B): every used parent fd
// reappears at the SAME number in the child, sharing the SAME open
// description (open_get per entry — shared offset/buffer/device, pipe
// side-alive flags untouched since no endpoint is created or destroyed).
// Two passes under one vfs_lock hold: first the child table must be
// completely free (stale occupant -> fail, never clobber), then install
// (open_get only — cannot fail, so no half-inherited table). The parent
// runs this call itself and only it mutates its table, except a racing
// kill-path close_all which serializes on the same lock either way.
// Returns 0 or -1.
int vfs_fork_inherit(int child, int parent) {
    if (child < 0 || child >= MAX_TASKS || parent < 0 || parent >= MAX_TASKS) return -1;
    if (child == parent) return -1;
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    int cbase = child * VFS_MAX_FDS;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (fds[cbase + i].used) {
            spinlock_unlock_irqrestore(&vfs_lock, f);
            return -1;
        }
    }
    int pbase = parent * VFS_MAX_FDS;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        vfs_fd_entry_t* pe = &fds[pbase + i];
        if (!pe->used || pe->owner != parent || !pe->open) continue;
        vfs_fd_entry_t* ce = &fds[cbase + i];
        open_get(pe->open);
        ce->used     = 1;
        ce->owner    = child;
        ce->fd_flags = pe->fd_flags;
        ce->open     = pe->open;
    }
    spinlock_unlock_irqrestore(&vfs_lock, f);
    return 0;
}

int vfs_close(int fd) {
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    int task_id = smp_current_task_id();
    int s = slot_of(task_id, fd);
    if (s < 0 || !fds[s].used || fds[s].owner != task_id || !fds[s].open) {
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return -1;
    }
    // Remove the task's reference first, then release the description:
    // the last close flushes + frees, earlier ones just drop the count.
    vfs_open_file_t* of = fds[s].open;
    fds[s].used     = 0;
    fds[s].open     = NULL;
    fds[s].fd_flags = 0;
    open_put(of);
    spinlock_unlock_irqrestore(&vfs_lock, f);
    return 0;
}

void vfs_close_all(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    int base = task_id * VFS_MAX_FDS;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        vfs_fd_entry_t* e = &fds[base + i];
        if (e->used && e->open) {
            // Same remove-then-release order as vfs_close: two entries
            // sharing one description put twice, and only the final put
            // flushes + frees. Another task's references are untouched.
            vfs_open_file_t* of = e->open;
            e->used     = 0;
            e->open     = NULL;
            e->fd_flags = 0;
            open_put(of);
        }
    }
    spinlock_unlock_irqrestore(&vfs_lock, f);
}

// Duplicate oldfd onto the lowest free fd in the CALLING task.
// Both entries reference the SAME open description (shared offset,
// shared buffer/dirty state, shared device) — nothing is copied.
// Returns the new fd (>= 0) or -1.
int vfs_dup(int oldfd) {
    int task_id = smp_current_task_id();
    if (task_id < 0 || task_id >= MAX_TASKS) return -1;
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    int os = slot_of(task_id, oldfd);
    if (os < 0 || !fds[os].used || fds[os].owner != task_id || !fds[os].open) {
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return -1;
    }
    int nfd = alloc_fd(task_id);
    if (nfd < 0) {
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return -1;   // fd table full
    }
    install_locked(task_id, nfd, fds[os].open);
    spinlock_unlock_irqrestore(&vfs_lock, f);
    return nfd;
}

// Duplicate oldfd onto exactly newfd in the CALLING task.
// oldfd == newfd is a validated no-op. An already-open newfd is
// closed first (its description released, freed if last reference).
// The new reference is acquired BEFORE the target is released, so
// dup2(a, b) can never destroy the description out from under itself
// when both ends already share it. Returns newfd or -1.
int vfs_dup2(int oldfd, int newfd) {
    int task_id = smp_current_task_id();
    if (task_id < 0 || task_id >= MAX_TASKS) return -1;
    if (newfd < 0 || newfd >= VFS_MAX_FDS) return -1;
    uint64_t f = spinlock_lock_irqsave(&vfs_lock);
    int os = slot_of(task_id, oldfd);
    int ns = slot_of(task_id, newfd);
    if (os < 0 || ns < 0 ||
        !fds[os].used || fds[os].owner != task_id || !fds[os].open) {
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return -1;
    }
    if (oldfd == newfd) {
        spinlock_unlock_irqrestore(&vfs_lock, f);
        return newfd;
    }
    vfs_open_file_t* src = fds[os].open;
    open_get(src);   // acquire first: safe even if newfd shares src
    if (fds[ns].used && fds[ns].open) {
        vfs_open_file_t* old = fds[ns].open;
        fds[ns].used     = 0;   // briefly empty; table still under lock
        fds[ns].open     = NULL;
        fds[ns].fd_flags = 0;
        open_put(old);   // may flush + free the displaced description
    }
    fds[ns].used     = 1;
    fds[ns].owner    = task_id;
    fds[ns].fd_flags = 0;
    fds[ns].open     = src;   // install the pre-acquired reference
    spinlock_unlock_irqrestore(&vfs_lock, f);
    return newfd;
}
