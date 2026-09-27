# Architecture Overview

## What KyuzenOS Is

KyuzenOS is a monolithic, 64-bit, higher-half operating system written from
scratch in C, C++, and Rust with no standard library and no starter code. It
boots on both BIOS and UEFI through the Limine boot protocol and provides a
preemptively scheduled, symmetric-multiprocessing (SMP) kernel with
memory-isolated Ring-3 userspace, a composited windowing system, an
extent-based filesystem, and a TCP/IP stack.

Every pixel, window, and keystroke displayed by the system is produced by code
in this repository.

| Property | Value |
| --- | --- |
| Architecture | x86_64 |
| Kernel model | Monolithic, higher-half, single kernel image |
| Kernel base address | `0xFFFFFFFF80000000` |
| Boot protocol | Limine (protocol revision 3), hybrid BIOS + UEFI ISO |
| Executable format | ELF64 |
| Toolchain | Clang/LLVM, NASM, `ld.lld` |
| Language mix | C (kernel, most drivers), C++ (GUI toolkit, desktop, SDK), Rust (optional userspace apps) |
| Kernel version | `0.3.1` (`include/kyuzen_version.h`) |

## Design Principles

1. **The kernel is one address space.** All kernel code runs in Ring 0 and
   shares a single higher-half mapping. Isolation is provided to userspace,
   not between kernel components.
2. **Userspace is isolated per process.** Each userspace process has its own
   PML4 (top-level page table) and its own heap; the kernel half of every
   address space is shared by value.
3. **Everything funnels through one path.** Each subsystem has exactly one
   authoritative implementation: one ELF loader, one process-exit path, one
   syscall dispatcher, one compositor flush, one shell engine. Legacy
   entry points are retained deliberately and documented as such.
4. **Explicit over implicit.** Interfaces carry documented contracts: lock
   ordering, blocking rules, ownership, and buffer-size limits.
5. **Fail closed.** Drivers and hardware paths validate and reject rather than
   assume; unsupported hardware falls back to a software path.
6. **Honest documentation.** Unimplemented features are labelled as such. The
   code is the source of truth.

## System Layers

```text
┌─────────────────────────────────────────────────────────────────────┐
│ Applications (Ring 3)                                                │
│   calc, clock, notepad, terminal, viewer, fileman, settings,         │
│   taskmgr, browser, desktop, test suites                             │
├─────────────────────────────────────────────────────────────────────┤
│ Frameworks & Libraries                                               │
│   libdesktop (C++)  ·  widget toolkit (libui)  ·  XML UI  ·          │
│   color  ·  media/PNG  ·  text/FreeType  ·  C SDK  ·  C++ SDK        │
├─────────────────────────────────────────────────────────────────────┤
│ Syscall Boundary  (int 0x80 + boundary copy)                         │
├─────────────────────────────────────────────────────────────────────┤
│ Kernel (Ring 0)                                                      │
│   Process · Scheduler · Sync · VMM/PMM/Heap · VFS · KWM/Compositor   │
│   · Network glue · Panic · SMP                                       │
├─────────────────────────────────────────────────────────────────────┤
│ Drivers                                                              │
│   ATA · PS/2 keyboard/mouse · PCI · RTC · serial · PIT/LAPIC ·       │
│   e1000 NIC · VirtIO-GPU · Intel iGPU                                │
├─────────────────────────────────────────────────────────────────────┤
│ Hardware / Firmware                                                  │
│   Limine bootloader → kernel handoff (memory map, framebuffer)       │
└─────────────────────────────────────────────────────────────────────┘
```

## Address-Space Model

KyuzenOS is a **higher-half** kernel: the kernel image and its data live in
the upper canonical half of the 64-bit address space, while userspace occupies
the lower half.

```text
0x0000000000000000 ┌──────────────────────────────────────┐
                   │  (unmapped / non-canonical guard)     │
0x0000000004000000 │  userspace ELF image base             │
0x000000000BFC0000 │  userspace stack (256 KB)             │
0x000000000C000000 │  USER_STACK_TOP                       │
0x0000000010000000 │  userspace heap start (UHEAP_BASE)    │
0x0000000040000000 │  userspace heap end (UHEAP_END)       │
0x0000800000000000 │  end of userspace VA (PML4 index 256) │
                   │  ... canonical hole ...               │
0xFFFF800000000000 │  HHDM (direct physical map)           │
0xFFFF900000000000 │  kernel heap (HEAP_START_VADDR)       │
0xFFFFFFFF80000000 │  kernel image base (KERNEL_VMA)       │
0xFFFFFFFFFFFFFFFF └──────────────────────────────────────┘
```

- **Userspace** (`PML4[0..255]`): per-process page tables, one PML4 per
  address space. Only ELF segments, the user stack, and the user heap are
  mapped here.
- **Kernel** (`PML4[256..511]`): shared by value across every address space.
  Pre-populated at kernel init and never deep-copied when cloning a process.
- **HHDM** (Higher-Half Direct Map): physical memory mapped at a fixed offset,
  used by the kernel to access physical pages directly. The offset is provided
  by Limine.
- **Kernel heap**: a virtual region (`0xFFFF900000000000`) mapped with
  supervisor-only pages, so Ring-3 code cannot touch kernel allocations.

See [Memory Map](../reference/memory-map.md) for exact ranges and
[Memory Management](../kernel/memory.md) for the allocator internals.

## Boot Chain

```text
Firmware (BIOS / UEFI)
    └─→ Limine bootloader (limine.conf)
            ├─ loads kernel ELF at KERNEL_VMA (0xFFFFFFFF80000000)
            ├─ installs higher-half direct map (HHDM)
            ├─ passes memory map + framebuffer via the Limine protocol
            └─ loads modules (apps, assets, fonts) into memory
                    └─→ kernel_main()
                            ├─ console / serial init
                            ├─ memory: PMM → VMM → kernel heap
                            ├─ CPU: GDT/TSS, IDT, SMAP/SMEP, WP
                            ├─ drivers: PCI, ATA, PS/2, RTC, timers
                            ├─ subsystems: FS mount, network, GHAL, KWM
                            ├─ SMP bring-up (LAPIC, AP cores)
                            ├─ tasking init (Task 0 = kernel main)
                            └─ install modules into the filesystem,
                               then start the desktop / login
```

Details are in [Boot Process](boot.md).

## Concurrency Model

- **SMP**: up to 16 CPUs (`SMP_MAX_CPUS`). APs are brought up via LAPIC and
  execute the same kernel image.
- **Scheduling**: one run queue per CPU; tasks are placed on the least-loaded
  CPU and stolen when a queue is empty. See [Scheduler](../kernel/scheduler.md).
- **Preemption**: a timer interrupt (PIT on the boot CPU, LAPIC timer on APs)
  saves the full register frame and may switch to another task. There is no
  manual `switch_task()`.
- **Locking**: spinlocks with an IRQ-save discipline for cross-CPU data; a
  documented global lock order prevents deadlock. See
  [Synchronization](../kernel/synchronization.md).
- **Blocking**: a two-phase block (`block_prepare` then `block_park`) closes
  the lost-wakeup window for every blocking primitive.

## Protection & Isolation

- **Ring 3 userspace**: applications run at CPL 3 with their own page tables.
- **SMAP / SMEP**: enabled when the CPU supports them; user memory is only
  accessed inside explicit `stac`/`clac` windows.
- **WP**: write-protect is enforced (`CR0.WP`), so the kernel cannot write
  read-only pages.
- **Boundary copy**: every pointer from userspace is validated against the
  caller's page tables and range-checked before use; see
  [Syscalls](../kernel/syscalls.md).
- **Guard pages**: the user heap and kernel heap insert unmapped guard pages
  between regions so overflows fault instead of corrupting silently.

## Subsystems at a Glance

| Subsystem | Location | Documentation |
| --- | --- | --- |
| Process & scheduling | `kernel/sched/`, `kernel/proc/` | [Scheduler](../kernel/scheduler.md), [Processes](../kernel/processes.md) |
| Memory | `kernel/mm/` | [Memory Management](../kernel/memory.md) |
| Syscalls | `kernel/syscall/` | [Syscalls](../kernel/syscalls.md), [Syscall Table](../reference/syscalls.md) |
| Filesystem | `kernel/fs/` | [Filesystem](../filesystem/README.md) |
| Networking | `kernel/net/`, `drivers/net/` | [Networking](../networking/README.md) |
| Graphics | `graphics/`, `kernel/gfx/` | [Graphics](../graphics/README.md) |
| GUI | `libs/gui/`, `system/desktop/` | [GUI](../gui/README.md) |
| Panic handling | `kernel/panic/`, `kernel/debug/` | [Panic & Crash Handling](../kernel/panic.md) |
| SMP | `kernel/smp/`, `arch/x86/` | [Interrupts & Timers](../kernel/interrupts.md) |

## Related Documentation

- [Boot Process](boot.md)
- [Memory Management](../kernel/memory.md)
- [Scheduler](../kernel/scheduler.md)
- [Syscalls](../kernel/syscalls.md)
- [Building](../development/building.md) — to build and run the system
