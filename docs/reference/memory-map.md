# Memory Map

This document lists the physical and virtual address layout and the fixed
memory regions of KyuzenOS.

## Virtual Address Space

```text
0x0000000000000000 ┌────────────────────────────────────────────┐
                   │  unmapped / non-canonical guard             │
0x0000000004000000 │  userspace ELF image base                   │
                   │  (PT_LOAD segments: code, data, bss)        │
0x000000000BFC0000 │  userspace stack (256 KB)                   │
0x000000000C000000 │  USER_STACK_TOP                             │
0x0000000010000000 │  userspace heap start (UHEAP_BASE)          │
0x0000000040000000 │  userspace heap end (UHEAP_END)             │
0x0000800000000000 │  end of userspace VA (PML4 index 256)       │
                   │  ... canonical hole ...                     │
0xFFFF800000000000 │  HHDM (higher-half direct map)              │
0xFFFF900000000000 │  kernel heap (HEAP_START_VADDR)             │
0xFFFFFFFF80000000 │  kernel image base (KERNEL_VMA)             │
0xFFFFFFFFFFFFFFFF └────────────────────────────────────────────┘
```

### Userspace regions (`PML4[0..255]`, per process)

| Region | Range | Notes |
| --- | --- | --- |
| ELF image | `0x04000000` | Application code/data/bss |
| User stack | `0x0BFC0000`–`0x0C000000` | 256 KB (`USER_STACK_SIZE`) |
| User heap | `0x10000000`–`0x40000000` | 768 MB VA (`UHEAP_BASE`..`UHEAP_END`) |

### Kernel regions (`PML4[256..511]`, shared by value)

| Region | Base | Notes |
| --- | --- | --- |
| HHDM | `0xFFFF800000000000` | Direct physical map (Limine-provided offset) |
| Kernel heap | `0xFFFF900000000000` | `HEAP_START_VADDR`, supervisor-only |
| Kernel image | `0xFFFFFFFF80000000` | `KERNEL_VMA` |

## Physical Memory

| Property | Value |
| --- | --- |
| Page / frame size | 4096 bytes |
| Reserved low zone | `< 0x04800000` (72 MB) — never allocated or freed by the PMM |
| PMM granularity | One bit per 4 KB frame |

The PMM refuses to free frames below `0x04800000` to protect firmware/ROM pages
that are not owned by the kernel. See
[Memory Management](../kernel/memory.md).

## Disk Layout (KyuzenFS)

| Region | Location |
| --- | --- |
| Superblock | Block 0 |
| Block bitmap | From `block_bitmap_start` |
| Inode bitmap | From `inode_bitmap_start` |
| Inode table | From `inode_table_start` |
| Data blocks | From `data_blocks_start` |
| Crash dump | Last 8 sectors (`KZFS_CRASHDUMP_SECTORS`) |

See [KyuzenFS Format](../filesystem/format.md).

## GDT Layout

| Index | Selector | Segment |
| --- | --- | --- |
| 0 | — | Null |
| 1 | `0x08` | Kernel code |
| 2 | `0x10` | Kernel data |
| 3 | `0x18` / `0x1B` | User code (RPL 3) |
| 4 | `0x20` / `0x23` | User data (RPL 3) |
| 5+ | `TSS_SELECTOR(cpu)` | One TSS descriptor per CPU (16 bytes each) |

`GDT_ENTRIES = 5 + 2*SMP_MAX_CPUS` = 37. The BSP TSS selector is `0x28`.

## Intel GTT Reservation (2 MB window)

| Offset | Use |
| --- | --- |
| `0x00000` | Legacy BCS ring (32 KB) |
| `0x10000` | Legacy status |
| `0x20000` | Gen12 context (16 KB) |
| `0x24000` | Gen12 status |
| `0x25000` | Gen12 ring (16 KB) |
| `0x100000+` | Buffer bump area |

See [Graphics: Backends](../graphics/backends.md).

## Related Documentation

- [Memory Management](../kernel/memory.md)
- [Architecture Overview](../architecture/overview.md)
- [Constants & Limits](constants.md)
- [KyuzenFS Format](../filesystem/format.md)
