# Memory Management

KyuzenOS splits memory management into four cooperating layers: a physical
frame allocator, a virtual paging layer, a kernel heap, and a per-process user
heap. This document describes each.

## Overview

| Layer | Location | Responsibility |
| --- | --- | --- |
| PMM (physical memory manager) | `kernel/mm/pmm.c` | Allocate and free 4 KB physical frames |
| VMM (virtual memory manager) | `kernel/mm/paging.c` | Page tables, address spaces, mapping/unmapping |
| Kernel heap | `kernel/mm/heap.c` | `kmalloc`/`kfree`/`krealloc` for kernel allocations |
| User heap | `kernel/mm/uheap.c` | Per-process userspace heap backing `sys_alloc`/`sys_free` |

Constants:

| Constant | Value | Meaning |
| --- | --- | --- |
| `PAGE_SIZE` | 4096 | Page/frame size in bytes |
| `HEAP_START_VADDR` | `0xFFFF900000000000` | Kernel heap virtual base |
| `HEAP_MAGIC` | `0xDEADC0DE` | Kernel heap block header magic |
| `UHEAP_BASE` | `0x10000000` | User heap virtual base |
| `UHEAP_END` | `0x40000000` | User heap virtual end |
| `UHEAP_MAX_ALLOC` | 64 MiB | Largest single user allocation |
| Reserved low zone | `< 0x04800000` (72 MB) | Never allocated or freed by the PMM |

## Physical Memory Manager (PMM)

The PMM tracks physical frames with a bitmap: one bit per 4 KB frame. It is
initialized from the Limine memory map.

### Behavior

- **Allocation**: `pmm_alloc_page()` returns the next free frame using a
  next-free hint, skipping the reserved low zone.
- **Freeing**: `pmm_free_page()` returns a frame to the free list. It performs
  two safety checks:
  - **Double-free detection** — freeing an already-free frame is caught.
  - **Low-zone rejection** — frees below `0x04800000` (72 MB) are rejected and
    logged. This zone holds firmware/ROM pages that are not owned by the
    kernel; freeing them would poison the free list.
- **Ownership guard**: before freeing a mapped page, callers use
  `pmm_owns_page()` to confirm the frame was allocated by the PMM (Limine and
  ROM pages are not).

> **Why the low-zone guard exists:** an earlier defect freed Limine's
> user-range mappings, adding firmware/ROM pages to the free list. A later heap
> allocation was backed by SeaBIOS ROM (`phys 0xF8000`), so stores vanished.
> The guard is the permanent fix. See the
> [heap corruption post-mortem](../troubleshooting/2026-07-26-heap-corruption-bosd.md).

## Virtual Memory Manager (VMM)

The VMM manages 4-level x86_64 page tables.

### Address-space structure

- `PML4[0..255]` — **user half**, per-process. Each process has its own PML4.
- `PML4[256..511]` — **kernel half**, shared by value across all address
  spaces. Populated at kernel init and never deep-copied.

### Key operations

| Function | Purpose |
| --- | --- |
| `vmm_map_page_into` | Map a page into a specific address space |
| `vmm_unmap_page_from` | Unmap a page from a specific address space |
| `vmm_alloc_page_into` | Allocate a frame and map it |
| `paging_is_mapped_into` | Test whether a virtual page is mapped in an address space |
| `vmm_create_address_space` | Allocate a new PML4 with the shared kernel half |
| `vmm_clone_user_as` | Deep-copy the user half (used by `fork`) |
| `vmm_destroy_address_space` | Free user-half frames and tables |
| `vmm_switch_pml4` | Load a new CR3 |
| `vmm_switch_to_kernel_as` | Return to the kernel address space |

### Fork semantics

`fork()` performs a **full physical copy** of the user half, not copy-on-write.
COW was rejected during design because:

- No page refcounts exist.
- A page fault always panics (there is no write-fault hook to trigger a COW
  copy).
- TLB shootdown is local-only, so remote COW invalidation is unsolved.

`vmm_clone_user_as` therefore clones `PML4[0..255]` page by page, allocating a
fresh frame and copying 4 KB via the HHDM for each. Huge and empty entries are
skipped exactly as in `vmm_destroy_address_space`. The kernel half is shared by
value and never deep-copied.

### TLB handling

`vmm_tlb_shootdown` performs a local `invlpg` only. There is no cross-CPU IPI
shootdown; this is a known limitation (see
[Remaining limitations](#current-limitations)).

## Kernel Heap

The kernel heap is a first-fit allocator with block splitting and coalescing,
backed by VMM pages mapped **supervisor-only** (`US=0`), so Ring-3 code cannot
touch kernel allocations.

### Block layout

Every allocation has a header with `HEAP_MAGIC` (`0xDEADC0DE`), a size, and
free/used state. Alignment is 16 bytes.

### API

```c
void *kmalloc(size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t old_size, size_t new_size);
```

> Note: `krealloc` takes the old size explicitly (the kernel does not track it
> implicitly).

### Growth

`expand_heap` maps additional pages on demand. On partial failure it unmaps the
already-mapped pages, frees their frames, and rolls back the heap end pointer so
no frames leak.

## User Heap (uheap)

Each process has its own heap in its own address space, managed by
`kernel/mm/uheap.c`. The heap is exposed to userspace through the
`sys_alloc` / `sys_free` / `sys_realloc` syscalls (9 / 10 / 19).

### Region model

- State is stored per task: `uheap_brk` (0 = never allocated) and
  `uheap_regions`, a linked list of region nodes kept in the **kernel** heap.
- `uheap_alloc` grows the break forward only, mapping each page with user
  permissions (flags `7`) and zeroing it via the HHDM.
- A **guard page** is inserted after every region: `brk` advances by
  `pages*4096 + 4096`, and the extra page is never mapped. An overflow faults
  instead of corrupting adjacent memory.
- `uheap_free` validates ownership by matching the region base; foreign or
  stale pointers are ignored.
- `uheap_realloc` is in-place when shrinking, otherwise allocates, copies, and
  frees. The caller-supplied old size is **not trusted** — the kernel tracks
  the real size.
- `uheap_clone` deep-copies the region list for `fork`; the pages themselves
  need no work because `vmm_clone_user_as` already cloned them at the same
  virtual addresses.
- `uheap_reset` is called on `exec` to drop all regions.

### Limits

- `UHEAP_MAX_ALLOC` = 64 MiB per single allocation.
- The heap spans 768 MB of virtual address space (`0x10000000`–`0x40000000`).
- The break is monotonic within a process lifetime: `free` returns frames to
  the PMM but does not lower `brk`, so virtual address space is not reused until
  `exec`. An application that churns allocations may exhaust the 768 MB of VA
  even though physical frames are reclaimed.

## Address Space Lifecycle

```text
vmm_create_address_space()
    └─ new PML4, kernel half shared
elf_load_file(..., target_pml4)
    └─ map ELF segments, user stack at 0x0BFC0000..0x0C000000
uheap_alloc()
    └─ map user heap pages on demand
... process runs ...

fork:
    vmm_clone_user_as() + uheap_clone()

exit:
    vmm_destroy_address_space()
        └─ free user-half frames and page tables
        └─ uheap_reset()
```

## Current Limitations

- **No COW**: `fork` copies all user pages physically.
- **No cross-CPU TLB shootdown**: unmapping a page on one CPU does not
  invalidate other CPUs' TLB entries via IPI. This is a pre-existing limitation
  shared by all unmaps.
- **User VA is not reclaimed** until `exec`.
- **No shared memory / `mmap` / ASLR.**
- The reserved low 72 MB is never available to the PMM.

## Development Notes

- Always call `pmm_owns_page()` before freeing a frame reached through a
  mapping.
- Kernel heap pages are `US=0`; do not attempt to map them into a user address
  space without changing flags.
- The VMM is not thread-safe by itself; callers hold `paging_lock` where
  required, and the syscall layer relies on the fact that only the owning task
  can modify its own address space.

## Related Documentation

- [Architecture Overview](../architecture/overview.md) — address-space diagram
- [Memory Map](../reference/memory-map.md) — exact ranges
- [Process Model](processes.md) — fork/exec and address-space lifecycle
- [Syscalls](syscalls.md) — boundary copy and user-pointer validation
