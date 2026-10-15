# R3 Sharing — Shared Surfaces, Process Isolation, Live GPU Sync

Prerequisite: R1 (`r1-contracts.md`) + R2 (`r2-integration.md`).
The kernel compositor remains the sole scanout owner. No triple buffering.

## 1. Capability audit (source-backed)

| Primitive | Status | Source |
|---|---|---|
| Process create/exit/cleanup (incl. fd/event/KWM teardown) | implemented, usable | `kernel/proc/proc.c:291-304` |
| Fork (full-copy AS) | implemented, WAS BROKEN, fixed in R3 | `kernel/mm/paging.c:596-642`, §2 |
| Per-task AS, 4KB map/unmap/protect | implemented, usable | `kernel/mm/paging.c:149-440` |
| PMM 4KB alloc/free | implemented, usable | `kernel/mm/pmm.c:214-260` |
| Syscall dispatch + boundary copy | implemented, usable | `kernel/syscall/syscall.c:41-149` |
| Pipes (4KB, fd-inheritable) | implemented, usable | `kernel/fs/vfs_fd.c:81-89,582` |
| Event queue (kernel→task only) | implemented, incomplete for IPC | `kernel/sync/event.c:40-81` |
| User mutex/sem/condvar syscalls | not implemented (kernel-only) | `include/sync.h:7-44` |
| shm/mmap/message-queue | NOT implemented → built minimal shm (§3) | — |
| VirtIO fence submit/poll/watermark | implemented, usable | `drivers/graphics/hw/virtio_gpu_dev.c:352-694` |
| Cross-CPU TLB shootdown | NOT implemented (local-only) | `kernel/mm/paging.c:68-71` |

Consequence: no cross-CPU unmap is ever performed (destroy retires
lazily); fresh mappings need no shootdown (VA was absent).

## 2. Pre-existing fork killer (fixed, required for R3)

`vmm_clone_user_as` read source frames via `e1 & PAGE_MASK`, which keeps
the NX bit (bit 63 is a flag, not address — the old comment claimed
`~PAGE_MASK` kept NX; it keeps only the low 12 bits). `PHYS_TO_VIRT`
then wrapped to non-canonical (`0x7FFF...`) and `memcpy` took `#GP`.
Every fork of a task with NX user pages (the stack always is) panicked.

Proven pre-existing: stashed R3, rebuilt clean `b1598dd`, `start
fork_test` panicked with the identical RIP/mode (`memcpy`,
`RSI=0x7FFF...`). After the fix: `FORK TEST PASS`, `PIPE TEST PASS`.

Fix (no ABI change): strip NX from frame addresses at the three leaf
PTE-phys readers (clone src, unmap return, destroy free) and preserve NX
in the flags propagated to the child (which previously lost W^X too).
`protect` (line ~383) still cannot clear NX — noted, untouched.

## 3. Shared-surface store (`include/kz_shm.h`, `kernel/mm/kz_shm.c`)

16 slots × 8 MiB (max 2048 pages), fixed VA `0x50000000 + id*8MiB`
(same in every AS; above uheap, below the canonical hole). Pages zeroed
at alloc (no cross-task leak). CPU pixels always caller-borrowed for
adapter surfaces and kernel-owned here — never raw kernel pointers
(userspace sees handles + granted VAs only).

Rights: owner RW; non-owner RO and only after publish; publish/destroy
owner-only (`KZ_EACCES` otherwise); metadata immutable after create.
Destroy = lazy retire: handle dies at once (`KZ_ESTALE`), pages live
until the last mapping drops (unmap/exit). `kz_shm_task_exit` (hooked
in `proc_do_exit`) drops mappings and destroys owned resources.
Syscalls 88/91/92/93/94 (`kernel/syscall/sys_shm.c`, userlib wrappers).

Fork exclusion: `vmm_clone_user_as` and `vmm_destroy_address_space` skip
`[KZ_SHM_BASE, KZ_SHM_END)` per-PTE (2 MiB-aligned, never straddles).
The child re-maps explicitly — a copied page would look shared but go
stale. `uheap` never enters the range (caps at `0x40000000`).

Sync (R1 stages, CPU path): create+write = producer access, `publish` =
SUBMITTED, consumer `unmap` = CONSUMER_DONE, slot free = RELEASED.
No GPU backing in R3 (a GPU resource has no mappable CPU pointer);
`kz_adapter` quarantine remains the GPU-side pattern.

## 4. Protocol over pipes (`kz_shm_msg_t`, 52 bytes, v1)

PUBLISH (handle as id+gen + damage + producer-attested dims) / RELEASE /
RESIZE / BYE. Receiver re-validates magic, version, handle, generation,
rights. Dims ride the message (no metadata syscall yet — R4); the
consumer clips all use to mapped pages and the differential checksum
catches lies (a wild lie faults inside the consumer only, never the
kernel). Backpressure = pipe semantics (4 KiB, blocking, EOF).

## 5. Out-of-process shadow (`tests/target/shm_demo.c` + `shadow_cons.c`)

One ELF, two processes via `fork` (fd table inherited, like `pipe_test`):
parent produces + publishes + speaks the protocol (malformed-first,
unpublished-handle, wild-handle, real frame); the child maps RO, verifies
pixels directly AND via `kz_shadow_compose` (same `libs/render` sources
as the host tests) into its own heap buffer, and answers with both
checksums. Nothing is presented; the kernel compositor owns scanout.

QEMU evidence (`tests/host/probes/_shm_probe.py`, virtio-vga):
`SHM DEMO PASS`, `DIRECT CK == SHADOW CK (859898437)`, `FENCE LIVE`
(present advanced, err 0), no panic markers. The `-vga std` software
path reports `FENCE SKIP` by design (no backend stats).

## 6. Tests

- Host `test-kz-shm` (real `kz_shm.c` vs RAM pmm/paging model): create,
  caps, table-full, rights matrix, stale/double-destroy, publish bounds,
  unmap bounds, lazy retire, exit cleanup, partial-init rollback,
  map-failure rollback, protocol layout, leak balance. New.
- QEMU `shm_demo`: 20+ in-OS checks across two processes.
- QEMU `fork_test`, `pipe_test`: pass (prerequisite + regression).
- Host suite: 28 passed / 3 disabled (pre-existing, unrelated).
- Probe needs the spawn-needle relaxation (SMP serial interleave splits
  `start: ...elf`; uses `DIRECT CK` as liveness instead — same documented
  flake class as `boot_check`'s `PAST_MARKER`).

## 7. Compatibility (diff guard)

Unchanged: `include/kwm_abi.h`, `kernel/gfx/compositor.c`,
`kernel/gfx/kwm.c`, `graphics/*`, `drivers/*`, `kernel/panic/*`,
`libs/gui/*`, `system/desktop/*`, Intel tree. New syscalls only (no
renumbering). Software fallback intact.

## 8. Deferred (R4)

Metadata syscall (dims/rights query by handle); GPU-backed shared
surfaces (needs mappable GPU memory or adapter-mediated upload);
consumer-acknowledged replacement; `protect` NX clearing; cross-CPU
shootdown if forced unmap is ever required.
