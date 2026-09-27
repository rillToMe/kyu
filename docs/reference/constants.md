# Constants & Limits

A consolidated reference of compile-time constants across KyuzenOS subsystems.
Each entry cites the defining header or source file.

## Kernel

| Constant | Value | Source |
| --- | --- | --- |
| `KYUZEN_VERSION` | `"0.3.1"` | `include/kyuzen_version.h` |
| `KERNEL_VMA` | `0xFFFFFFFF80000000` | `linker.ld` |
| `PAGE_SIZE` | 4096 | memory |
| `HEAP_START_VADDR` | `0xFFFF900000000000` | `kernel/mm/heap.c` |
| `HEAP_MAGIC` | `0xDEADC0DE` | `kernel/mm/heap.c` |
| Reserved low zone | `< 0x04800000` (72 MB) | `kernel/mm/pmm.c` |

## Tasks & Scheduler

| Constant | Value | Source |
| --- | --- | --- |
| `MAX_TASKS` | 16 | `include/task.h` |
| `TASK_STACK_SIZE` | 16384 | `include/task.h` |
| `SYSCALL_STACK_SIZE` | 16384 | `kernel/sched/lifecycle.c` |
| `SMP_MAX_CPUS` | 16 | SMP |
| `RUNQ_CAPACITY` | `MAX_TASKS` (16) | `kernel/sched/runqueue.c` |
| `AGE_STEP_MS` | 100 | `kernel/sched/runqueue.c` |
| `AGE_MAX_BONUS` | 8 | `kernel/sched/runqueue.c` |
| Scheduler quantum | 20 ms | `drivers/timer.c` |
| Timer refresh | 60 / 100 / 144 Hz (default 60) | `include/timer.h` |

Task states: `TASK_READY 0`, `TASK_RUNNING 1`, `TASK_SLEEPING 2`,
`TASK_DEAD 3`, `TASK_BLOCKED 4`, `TASK_ZOMBIE 5`.

## Process

| Constant | Value | Source |
| --- | --- | --- |
| `PROC_NO_PARENT` | -1 | `include/proc.h` |
| `PROC_MAX_ARGC` | 16 | `include/proc.h` |
| `PROC_MAX_ARG_LEN` | 64 | `include/proc.h` |
| `PROC_ARG_TOTAL_MAX` | 512 | `include/proc.h` |
| `PROC_WAIT_ANY` | -1 | `include/proc.h` |
| `PROC_WNOHANG` | 1 | `include/proc.h` |
| `PROC_KILL_EXIT_CODE` | 125 | `include/proc.h` |
| `PROC_EXIT_NORMAL` | 0 | `include/proc.h` |
| `PROC_EXIT_KILLED` | 1 | `include/proc.h` |

## Memory (Userspace)

| Constant | Value | Source |
| --- | --- | --- |
| `USER_STACK_TOP` | `0x0C000000` | `include/elf.h` |
| `USER_STACK_SIZE` | 256 KB | `include/elf.h` |
| `UHEAP_BASE` | `0x10000000` | `include/uheap.h` |
| `UHEAP_END` | `0x40000000` | `include/uheap.h` |
| `UHEAP_MAX_ALLOC` | 64 MiB | `include/uheap.h` |

## Syscall Boundary

| Constant | Value | Source |
| --- | --- | --- |
| `UC_USER_VA_MAX` | `0x0000800000000000` | `include/usercopy.h` |
| `UC_MAX_RANGE` | 64 MiB | `include/usercopy.h` |
| `UC_MAX_STR` | 1024 | `include/usercopy.h` |
| `UC_MAX_FNAME` | 64 | `include/usercopy.h` |
| `UC_MAX_HOST` | 128 | `include/usercopy.h` |
| `UC_MAX_KBD` | 512 | `include/usercopy.h` |
| `UC_MAX_FILE` | 8 MiB | `include/usercopy.h` |
| `UC_MAX_IO` | 1 MiB | `include/usercopy.h` |
| `UC_MAX_SOCK` | 64 KiB | `include/usercopy.h` |
| `UC_MAX_ENTRIES` | 128 | `include/usercopy.h` |

## Filesystem (KyuzenFS)

| Constant | Value | Source |
| --- | --- | --- |
| `KZFS_MAGIC` | `0x53465A4B` | `include/kyuzenfs_v4.h` |
| `KZFS_VERSION` | `0x00040000` | `include/kyuzenfs_v4.h` |
| `KZFS_BLOCK_SIZE` | 4096 | `include/kyuzenfs_v4.h` |
| `KZFS_BLOCK_SECTORS` | 8 | `include/kyuzenfs_v4.h` |
| `KZFS_SECTOR_SIZE` | 512 | `include/kyuzenfs_v4.h` |
| `KZFS_INODE_SIZE` | 128 | `include/kyuzenfs_v4.h` |
| `KZFS_INODES_PER_BLOCK` | 32 | `include/kyuzenfs_v4.h` |
| `KZFS_NAME_MAX` | 255 | `include/kyuzenfs_v4.h` |
| `KZFS_NUM_DIRECT_EXTENTS` | 4 | `include/kyuzenfs_v4.h` |
| `KZFS_EXTS_PER_IND_BLOCK` | 341 | `kernel/fs/kfs_extent.c` |
| `KZFS_MAX_EXTENTS` | 345 | `kernel/fs/kfs_extent.c` |
| `KZFS_PATH_MAX_DEPTH` | 32 | `kernel/fs/kfs_dir.c` |
| `KZFS_CRASHDUMP_SECTORS` | 8 | `include/kyuzenfs_v4.h` |
| `BCACHE_BLOCK_SIZE` | 4096 | `include/bcache.h` |
| `BCACHE_N_BLOCKS` | 256 (1 MB) | `include/bcache.h` |
| `VFS_MAX_FDS` | 16 | `include/vfs.h` |
| `VFS_MAX_PATH` | 23 | `include/vfs.h` |
| `VFS_PIPE_CAP` | 4096 | `include/vfs.h` |

## Graphics

| Constant | Value | Source |
| --- | --- | --- |
| `GHAL_MAX_BACKENDS` | 8 | `graphics/ghal.c` |
| `GHAL_MAX_DIM` | 8192 | `graphics/memory/gpu_alloc.h` |
| `MAX_DIRTY_REGIONS` | 64 | `include/display.h` |
| `PRESENT_UNION_MAX_INFLATION` | 2 | `kernel/gfx/compositor.c` |
| `KWM_MAX_OCCLUSION_RECTS` | 8 | `kernel/gfx/compositor.c` |
| `KWM_SPLIT_MIN_PIXELS` | 1024 | `kernel/gfx/compositor.c` |
| `KWM_MAX_OPAQUE_SPLITS` | 4 | `kernel/gfx/compositor.c` |
| `KWM_MAX_SPLIT_RECTS` | 12 | `kernel/gfx/compositor.c` |
| `CURSOR_WIDTH` / `CURSOR_HEIGHT` | 12 / 16 | `kernel/gfx/compositor.c` |
| `HW_CURSOR_SIZE` | 64 | `kernel/gfx/compositor.c` |
| `DISPLAY_MAX_DIM` | 8192 | `kernel/gfx/fb.c` |

## Window Manager

| Constant | Value | Source |
| --- | --- | --- |
| `MAX_WINDOWS` | 16 | `kernel/gfx/kwm_internal.h` |
| `KWM_MAX_DIMENSION` | 4096 | `kernel/gfx/kwm.c` |
| `KWM_MAX_CANVAS_BYTES` | 16 MiB | `kernel/gfx/kwm.c` |
| `KWM_TITLEBAR_H` | 32 | `kernel/gfx/kwm_internal.h` |
| `KWM_CLOSE_BTN_W` | 46 | `kernel/gfx/kwm_internal.h` |
| `KWM_CORNER_R` | 8 | `kernel/gfx/kwm_internal.h` |
| `KWM_SHADOW_MARGIN` | 6 | `kernel/gfx/kwm_internal.h` |
| `KWM_EDGE_ALPHA` | 70 | `kernel/gfx/kwm_internal.h` |
| `kwm_window_info_t` size | 68 bytes | `include/kwm_abi.h` |

## Intel GPU

| Constant | Value | Source |
| --- | --- | --- |
| `INTEL_GTT_MAX_ENTRIES` | 512 | `graphics/backend/intel/intel_regs.h` |
| `INTEL_GPU_PAGE_SIZE` | 4096 | Intel backend |
| `INTEL_BUF_MAX_PAGES` | 64 | Intel backend |
| `INTEL_MAX_BUFFERS` | 32 | Intel backend |
| `INTEL_BCS_RING_PAGES` | 8 | Intel backend |
| `INTEL_BCS_RING_SIZE` | 32 KB | Intel backend |

## VirtIO-GPU

| Constant | Value | Source |
| --- | --- | --- |
| `VG_VQ_MAX_SIZE` | 256 | `drivers/graphics/hw/virtqueue.h` |
| `VGPU_FENCE_MAX_HEADS` | 64 | VirtIO backend |
| `VGPU_ASYNC_PAIRS` | 8 | VirtIO backend |
| `VGPU_RESP_SYNC_MAX` | 512 | VirtIO backend |
| `VGPU_RESP_SLOT` | 64 | VirtIO backend |
| `VGPU_CURSOR_MAX_TIMEOUTS` | 3 | VirtIO backend |

## Networking

| Constant | Value | Source |
| --- | --- | --- |
| `NET_DNS_TIMEOUT_MS` | 5000 | `include/net_dns.h` |
| `NET_DNS_WAIT_QUANTUM_MS` | 20 | `include/net_dns.h` |
| `NET_DNS_MAX_HOST` | 128 | `include/net_dns.h` |
| `ENTROPY_MAX` | 256 | `include/entropy.h` |
| `ENTROPY_ERR` / `ENTROPY_ENOHW` | -1 / -2 | `include/entropy.h` |

## XML UI

| Constant | Value |
| --- | --- |
| `UI_XML_MAX_DOC` | 65536 |
| `UI_XML_MAX_DEPTH` | 16 |
| `UI_XML_MAX_NODES` | 512 |
| `UI_XML_MAX_ATTRS` | 16 |
| `UI_XML_MAX_CHILDREN` | 64 |
| `UI_XML_MAX_IDS` | 32 |
| `UI_XML_MAX_ROOTS` | 16 |

## C SDK Heap

| Constant | Value |
| --- | --- |
| `KYUZEN_LIBC_ARENA_BYTES` | 1 MiB |
| `KYUZEN_LIBC_ARENA_MAX_BYTES` | 4 MiB |
| `KYUZEN_LIBC_MAX_ARENAS` | 32 |
| `KYUZEN_LIBC_MAX_ALLOC_BYTES` | 64 MiB |

## Panic / Crash

| Constant | Value | Source |
| --- | --- | --- |
| `CRASHDUMP_MAGIC` | `0x44435A4B` | crash dump |
| `CRASHDUMP_VERSION` | 1 | crash dump |
| `CRASHDUMP_MAX_BYTES` | 4096 | crash dump |
| `CRASH_ARCHIVE_PATH` | `/crash-report.txt` | crash notice |

## Related Documentation

- [Memory Map](memory-map.md)
- [Syscall Reference](syscalls.md)
