#ifndef KZ_SHM_H
#define KZ_SHM_H

#include <stdint.h>

// ============================================================
// R3 Shared Surfaces (include/kz_shm.h) — shared kernel/userspace ABI.
//
// Kernel-mediated pixel sharing for separate processes. The kernel owns
// the pages; tasks map them at a fixed per-resource VA. No raw kernel
// pointers cross the boundary: userspace sees only (id+generation)
// handles and the mapped VA it was granted.
//
// --- Address layout ---
// Slot i maps at KZ_SHM_BASE + i*KZ_SHM_SLOT_SIZE in EVERY task that maps
// it (same VA everywhere: no per-task translation, fork-safe by exclusion).
// Above uheap ([0x10000000,0x40000000)), below canonical hole.
// --- Rights ---
// Owner: read+write. Others: read-only AND only after publish. Publish and
// destroy are owner-only. Metadata is immutable after create.
// --- Lifetime ---
// Destroy is lazy-retire (same pattern as the R2 adapter quarantine):
// the handle dies immediately (stale use -> KZ_ESTALE) but pages live
// until no mapping references them. No cross-CPU unmap is ever needed.
// --- Sync stages (R1 kz_sync, CPU path) ---
// publish = SUBMITTED (content complete, visible to consumers).
// Consumer unmap after reading = CONSUMER_DONE. Slot free = RELEASED.
// Software path completes synchronously; no GPU backing in R3 (a GPU
// resource has no mappable CPU pointer — see r3-sharing.md).
// ============================================================

#define KZ_SHM_BASE       0x50000000ULL
#define KZ_SHM_SLOT_SIZE  0x800000ULL   // 8 MiB per resource
#define KZ_SHM_MAX_RES    16
#define KZ_SHM_MAX_PAGES  2048          // 8 MiB / 4 KiB
#define KZ_SHM_PAGE_SIZE  4096ULL
#define KZ_SHM_END        (KZ_SHM_BASE + (uint64_t)KZ_SHM_MAX_RES * KZ_SHM_SLOT_SIZE)

#define KZ_SHM_MAP_WRITE  (1u << 0)     // request write (owner only)

// Syscalls (88 was empty; 91-94 taken at R3; 95-97 taken at R4).
#define SYS_SHM_CREATE   88  // RBX=w, RCX=h, RDX=format -> int64 handle / negative
#define SYS_SHM_MAP      91  // RBX=handle, RCX=flags -> user pointer / 0
#define SYS_SHM_UNMAP    92  // RBX=ptr -> 0 / -1
#define SYS_SHM_PUBLISH  93  // RBX=handle, RCX=x, RDX=y, RSI=w, RDI=h -> 0 / negative
#define SYS_SHM_DESTROY  94  // RBX=handle -> 0 / negative
#define SYS_SHM_STAT     95  // RBX=handle, RCX=out(kz_shm_stat_t*) -> 0 / negative
#define SYS_SHM_ACQUIRE  96  // RBX=handle -> 0 / negative (consumer ref)
#define SYS_SHM_RELEASE  97  // RBX=handle -> 0 / negative (drop consumer ref)

// Handle: high 32 = generation (never 0 live), low 32 = slot index.
// Negative int64 return = -kz_status (KZ_EINVAL/ESTALE/EACCES/EBUSY/ENOMEM).
static inline uint64_t kz_shm_handle(uint32_t gen, uint32_t idx) {
    return ((uint64_t)gen << 32) | idx;
}
static inline uint32_t kz_shm_idx(uint64_t h) {
    return (uint32_t)(h & 0xFFFFFFFFu);
}
static inline uint32_t kz_shm_gen(uint64_t h) {
    return (uint32_t)(h >> 32);
}
static inline uint64_t kz_shm_slot_va(uint32_t idx) {
    return KZ_SHM_BASE + (uint64_t)idx * KZ_SHM_SLOT_SIZE;
}

// Extra error: access denied (wrong owner / unpublished). Matches the
// KSOCK-style negative-errno convention, distinct from KZ_ESTATE.
#define KZ_EACCES (-8)

// --- Authoritative metadata (R4) ---
// Filled by the kernel from the actual allocation, never from producer
// messages. Only visible to the owner or (after publish) to consumers.
// Destroyed resources report KZ_ESTALE, never stale metadata.
#define KZ_SHM_STAT_VER 1
#define KZ_SHM_STATE_ACTIVE 1
#define KZ_SHM_STATE_PUBLISHED 2
#define KZ_SHM_RIGHT_READ (1u << 0)
#define KZ_SHM_RIGHT_WRITE (1u << 1)

typedef struct {
    uint32_t version;     // KZ_SHM_STAT_VER
    uint32_t width, height, stride, format;
    uint64_t size_bytes;  // stride * height * 4, overflow-checked at create
    uint32_t generation;  // live generation (matches the handle)
    uint32_t owner;       // owning task id
    uint32_t rights;      // KZ_SHM_RIGHT_* for the CALLER
    uint32_t state;       // KZ_SHM_STATE_*
    uint32_t npages;      // allocated pages
} kz_shm_stat_t;

// Range predicate shared by paging.c (clone/destroy exclusion) and tests.
static inline int kz_shm_va_excluded(uint64_t va) {
    return va >= KZ_SHM_BASE && va < KZ_SHM_END;
}

// --- Kernel operations (kernel/mm/kz_shm.c) ---
// pml4 is the caller's address-space root (uint64_t to keep this header
// includable from userspace; kernel passes phys_addr_t). All ops run in
// the caller's syscall context and map only the caller's own AS.
int64_t kz_shm_create(int32_t task, uint64_t pml4, uint32_t w, uint32_t h,
                       uint32_t format);
uint64_t kz_shm_map(int32_t task, uint64_t pml4, uint64_t handle,
                     uint32_t flags);
int32_t kz_shm_unmap(int32_t task, uint64_t pml4, uint64_t va);
int32_t kz_shm_publish(int32_t task, uint64_t handle, int32_t x, int32_t y,
                        int32_t w, int32_t h);
int32_t kz_shm_destroy(int32_t task, uint64_t handle);
int32_t kz_shm_stat(int32_t task, uint64_t handle, kz_shm_stat_t *out);
int32_t kz_shm_acquire(int32_t task, uint64_t handle);
int32_t kz_shm_release(int32_t task, uint64_t handle);
// Kernel-internal consume: acquire a consumer ref and borrow the page
// array (stable until the matching release). Pages are kernel phys;
// map them with HHDM, never expose them to userspace.
int32_t kz_shm_consume(int32_t task, uint64_t handle, uint64_t **pages_out,
                        uint32_t *npages_out, uint32_t *stride_out,
                        uint32_t *width_out, uint32_t *height_out,
                        uint32_t *format_out);
void kz_shm_task_exit(int32_t task);

// --- Cross-process protocol over pipes (Task D) ---
// Fixed 52-byte messages on a byte-stream pipe. Handles travel as (id,gen);
// raw pointers NEVER cross processes. Receiver re-validates everything.
#define KZ_SHM_MSG_MAGIC 0x53484D50u   // 'SHMP'
#define KZ_SHM_MSG_VER   1u

typedef enum {
    KZ_SHM_MSG_PUBLISH = 1,  // frame ready: handle + damage
    KZ_SHM_MSG_RELEASE = 2,  // consumer done with handle
    KZ_SHM_MSG_RESIZE  = 3,  // old handle retired, new handle follows
    KZ_SHM_MSG_BYE     = 4,  // producer exiting, no more frames
} kz_shm_msg_type_t;

typedef struct {
    uint32_t magic;    // KZ_SHM_MSG_MAGIC
    uint32_t version;  // KZ_SHM_MSG_VER
    uint32_t type;     // kz_shm_msg_type_t
    uint32_t gen;      // handle generation
    uint32_t id;       // handle slot
    int32_t x, y;      // damage origin (PUBLISH)
    int32_t w, h;      // damage size   (PUBLISH)
    uint32_t width;    // surface dims (producer-attested; R4 adds a
    uint32_t height;   // metadata syscall — consumer clips all use to the
    uint32_t format;   // mapped pages and the differential checksum catches
    uint32_t stride;   // any lie; a wild lie faults inside the consumer only)
} kz_shm_msg_t;

#endif
