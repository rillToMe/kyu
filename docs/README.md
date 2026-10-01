# KyuzenOS Documentation

This directory contains the technical documentation for KyuzenOS, a 64-bit
higher-half operating system written from scratch in C, C++, and Rust.

The documentation is organized by **system and subsystem**, not by development
phase. Every document describes the current implementation; where a feature is
incomplete or unsupported, that is stated explicitly. When documentation and
code disagree, the code is authoritative — please report the discrepancy.

> **New here?** Start with the [project README](../README.md) for an overview,
> then read [Architecture Overview](architecture/overview.md).

---

## Documentation Map

### Architecture

The big picture: how the pieces fit together.

| Document | Description |
| --- | --- |
| [Overview](architecture/overview.md) | System layers, boot chain, address-space model, design principles |
| [Boot Process](architecture/boot.md) | Limine handoff, early initialization order, module installation |

### Kernel

| Document | Description |
| --- | --- |
| [Memory Management](kernel/memory.md) | Physical allocator (PMM), virtual paging (VMM), kernel heap |
| [Scheduler](kernel/scheduler.md) | Per-CPU run queues, priorities, work stealing, context switching |
| [Process Model](kernel/processes.md) | Task lifecycle, fork/exec/spawn, wait, kill, credentials |
| [Synchronization](kernel/synchronization.md) | Spinlocks, mutex/semaphore/condvar, wait queues, blocking model |
| [Syscalls](kernel/syscalls.md) | The `int 0x80` ABI, argument passing, dispatch, boundary copy |
| [Interrupts & Timers](kernel/interrupts.md) | IDT, ISRs, PIT/LAPIC, SMP bring-up |
| [Panic & Crash Handling](kernel/panic.md) | BSOD, crash dump, recovery path |

### Userspace

| Document | Description |
| --- | --- |
| [Userspace Model](userspace/overview.md) | Ring-3 isolation, per-process address spaces, SMAP/SMEP |
| [Applications](userspace/applications.md) | Bundled apps, manifests, how to add a new application |
| [Shell & CLI](userspace/shell.md) | Command shell, builtins, redirection, pipelines |

### Subsystems

| Document | Description |
| --- | --- |
| [Filesystem](filesystem/README.md) | KyuzenFS V4 on-disk format, block cache, VFS layer, file descriptors |
| [Networking](networking/README.md) | lwIP integration, e1000 driver, socket layer, DNS |
| [Graphics](graphics/README.md) | Graphics HAL, compositor, dirty regions, backends (software, VirtIO, Intel) |
| [GUI](gui/README.md) | Widget toolkit, XML UI, libdesktop framework, desktop environment |
| [Browser](browser/README.md) | HTML/CSS engine, HTTP, TLS |

### Libraries & Toolchain

| Document | Description |
| --- | --- |
| [C SDK](libraries/c-sdk.md) | LLVM libc 22 integration, heap, port layer, limitations |
| [C++ SDK](libraries/cpp-sdk.md) | libc++ subset, C++ runtime, the `kyuzen-c++` wrapper |
| [Rust Support](libraries/rust.md) | `no_std` userspace, syscall bindings, Slint integration |
| [Shared Libraries](libraries/shared.md) | Color library, media/PNG decoder, text/FreeType rendering |

### Development

| Document | Description |
| --- | --- |
| [Building](development/building.md) | Toolchain setup, build targets, platform notes |
| [Running](development/running.md) | QEMU configuration, first boot, credentials |
| [Testing](development/testing.md) | Host tests, QEMU probes, in-OS test suites |
| [Debugging](development/debugging.md) | Serial logging, BSOD, crash reports, watchpoints |

### Contributing

| Document | Description |
| --- | --- |
| [Contributing Guide](contributing/README.md) | Workflow, conventions, review checklist |

### Reference

| Document | Description |
| --- | --- |
| [Syscall Table](reference/syscalls.md) | Complete syscall number/argument/return reference |
| [Memory Map](reference/memory-map.md) | Physical and virtual address layout, limits |
| [Constants & Limits](reference/constants.md) | Compile-time limits across all subsystems |

### Design Notes

Stable design rationale that does not fit a single subsystem:

| Document | Description |
| --- | --- |
| [Design Decisions](design/README.md) | Index of architecture decision records |

### Planning

Forward-looking plans and the project-level roadmap:

| Document | Description |
| --- | --- |
| [OSDev Portfolio Roadmap](ROADMAP-OSDEV.md) | Phased plan to reach reproducible-build, CI, stability, FPU/SSE, and a defined technical angle |

### History

| Document | Description |
| --- | --- |
| [Development History](history/README.md) | Project milestones and completed work phases |
| [Bug Post-Mortems](troubleshooting/) | In-depth incident investigations |

---

## Documentation Conventions

- **Language**: English. Identifiers, paths, commands, and log output are kept
  verbatim.
- **Status labels**: `Currently supported`, `Not currently implemented`,
  `Experimental`, and `Planned` are used only where the repository supports
  them.
- **Source references**: Documents cite concrete files and constants. If a
  citation is wrong, the code wins.
- **One topic, one document**: Related material is consolidated rather than
  split across many small files.
