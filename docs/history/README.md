# Development History

This directory preserves the project's historical development records. It is
kept for context and traceability; the primary documentation describes the
**current** system, not the path taken to reach it.

Historical documents are organized by area:

| Directory | Contents |
| --- | --- |
| [`graphics/`](graphics/) | Graphics/compositor per-phase performance and status reports, plus superseded pre-GHAL design documents |
| [`gui/`](gui/) | GUI development-phase documents (desktop environment, toolkit, interaction, polish, layout, XML) |
| [`ring3/`](ring3/) | Ring-3 migration records (CPL3 entry, boundary copy, user heap, SMAP/SMEP) |
| [`misc/`](misc/) | Other development-phase notes (for example, filesystem directory phases) |

## Milestones

KyuzenOS grew from a minimal kernel into a full system with a composited
desktop, networking, and a browser. The high-level arc, reconstructed from the
repository history and the historical documents here:

1. **Kernel foundation** — boot via Limine, physical and virtual memory,
   interrupts, a multitasking scheduler, and an interactive shell with a
   dynamic memory allocator and a text editor.
2. **Filesystem and process model** — a from-scratch filesystem (KyuzenFS,
   now V4) with directories, and a unified process model (spawn/exec/fork/wait/
   kill) with credentials.
3. **Graphics and windowing** — the compositor, the KWM window manager, the
   Graphics HAL (GHAL), and the dirty-region/occlusion optimizations.
4. **GUI stack** — the widget toolkit (`libui`), the theme system, the
   `libdesktop` C++ framework, the desktop environment, and XML declarative UI.
5. **Userspace hardening** — the Ring-3 migration (CPL3 isolation, boundary
   copy, user heap, SMAP/SMEP), the C and C++ SDKs, and Rust support.
6. **Networking and browser** — lwIP integration, the e1000 driver, the socket
   layer, DNS, and a from-scratch browser with BearSSL TLS.
7. **Acceleration** — the VirtIO-GPU and Intel integrated GPU backends.

## Reading Historical Documents

- These documents use "phase" terminology from the project's internal
  planning. That terminology is intentionally **not** used in the primary
  documentation.
- Where a historical document describes behavior that has since changed (for
  example, the pre-GHAL graphics API), the subsystem documentation under
  `docs/graphics/` reflects the current implementation.
- Genuinely valuable design rationale from these documents has been folded into
  [Design Notes](../design/README.md) and the subsystem documents.

## Incident Post-Mortems

Detailed bug investigations are kept separately under
[`docs/troubleshooting/`](../troubleshooting/), which includes the heap
corruption investigation and the Ring-3 / SMP bring-up records.

## Related Documentation

- [Design Notes](../design/README.md)
- [Documentation Index](../README.md)
- [Architecture Overview](../architecture/overview.md)
