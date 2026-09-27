# Userspace Model

KyuzenOS runs applications in **Ring 3** (CPL 3) with per-process address
spaces. This document describes the isolation model, how a task enters and
leaves userspace, and how the boundary between userspace and kernel is
enforced.

## Isolation

Each userspace process has:

- **Its own PML4** (top-level page table), isolating its address space.
- **Its own user heap** (`sys_alloc` / `sys_free` / `sys_realloc`).
- **Its own file descriptor table** and event queue.
- **Task-local credentials** (`uid`/`gid`).

The kernel half of every address space is shared by value; only the user half
is private. See [Memory Management](../kernel/memory.md) for the layout.

## Entering Ring 3

A task enters userspace by constructing an interrupt-return frame that returns
to CPL 3:

```c
r->rip = entry;              // ELF entry point
r->rsp = user_stack_top;
r->cs  = 0x1B;               // user code segment | RPL 3
r->ss  = 0x23;               // user data segment | RPL 3
r->rdi = argc;
r->rsi = argv;
```

The `iretq` at the end of the syscall stub performs the transition. The GDT
selectors are:

| Selector | Segment |
| --- | --- |
| `0x08` | Kernel code |
| `0x10` | Kernel data |
| `0x1B` | User code (RPL 3) |
| `0x23` | User data (RPL 3) |

There are two entry paths:

- `sys_exec` (33) — in-place replacement of the calling task's image.
- `create_user_task` — used by spawn (57/68/77) and fork (78), building a fresh
  frame.

## Kernel Stacks and RSP0

Interrupts and syscalls from Ring 3 switch to a kernel stack via TSS.RSP0.

- Each CPU has a dedicated **syscall stack** (`SYSCALL_STACK_SIZE` = 16 KB).
- On every context switch, RSP0 is repointed to the **incoming task's** kernel
  stack (see [Scheduler](../kernel/scheduler.md)). This is required so that two
  Ring-3 tasks sharing a CPU do not overwrite each other's preemption frames.

## Protection Mechanisms

| Mechanism | Effect |
| --- | --- |
| Ring 3 | User code cannot execute privileged instructions or touch kernel memory |
| SMEP | Kernel cannot execute user pages |
| SMAP | Kernel cannot access user data outside explicit windows |
| WP | Kernel cannot write read-only pages |
| Boundary copy | Every user pointer is validated before use |
| Guard pages | User heap overflows fault instead of corrupting |

See [Syscalls](../kernel/syscalls.md) for the boundary-copy contract and the
SMAP windows.

## Userspace Libraries

| Library | Location | Purpose |
| --- | --- | --- |
| `userlib` | `libs/core/userlib.c` | Syscall wrappers (the base for everything else) |
| `libgui` | `libs/core/libgui.c` | Legacy GUI helpers |
| `userutil` | `libs/core/userutil.c` | Utility helpers |
| `netutil` | `libs/core/netutil.c` | Address parsing, resolve, dial |
| Widget toolkit | `libs/gui/widget/` | GUI widgets (C ABI) |
| libdesktop | `libs/gui/libdesktop/` | C++ desktop framework |
| color | `libs/gui/color/` | Color types and blending |
| media | `libs/media/` | PNG/BMP decoding |
| text | `libs/text/` | FreeType-backed text rendering |

## Related Documentation

- [Applications](applications.md) — bundled apps and how to add one
- [Shell & CLI](shell.md) — the command shell
- [Syscalls](../kernel/syscalls.md) — the ABI and boundary copy
- [Memory Management](../kernel/memory.md) — user heap and address spaces
- [C SDK](../libraries/c-sdk.md), [C++ SDK](../libraries/cpp-sdk.md)
