# Scope

What KyuzenOS is, what it deliberately is **not**, and why each boundary was
drawn. This document exists so that a reader can tell the difference between
"not finished yet" and "not going to happen" without asking.

Where this document and the code disagree, the code wins. Where it and a
subsystem document disagree, that document is describing a specific subsystem
in more detail and is more precise about it.

---

## What KyuzenOS is

A 64-bit higher-half monolithic operating system written from scratch in C,
C++, and Rust. It boots on bare metal (BIOS and UEFI) and in QEMU, and it
provides:

- A preemptive, SMP-aware scheduler (up to 16 CPUs, per-CPU run queues with
  work stealing).
- Ring-3 isolation for **all** user code, including `init`, `login` and the
  shell — the kernel image contains no user code.
- A from-scratch extent-based filesystem (KyuzenFS V4) with a real directory
  tree and a POSIX-style fd API.
- A composited window manager and a widget toolkit, with its own desktop
  environment.
- TCP/IP through lwIP on an Intel e1000 NIC: DHCP, DNS, ICMP, TCP client
  sockets.
- C, C++ and (optional) Rust userspace SDKs.

The intended use is as a **technical demonstration and a learning vehicle**: a
system small enough for one person to understand end to end, and complete
enough to run a GUI with several applications at once.

## What KyuzenOS is not

Each item below is a deliberate decision, not an omission. The alternative
would consume the project without making it better at what it is for.

### Not a Linux replacement, and not a daily driver

There is no plan to make KyuzenOS a general-purpose desktop OS. That requires a
team, a decade, and hardware support far beyond what is here — and it would
replace the project's actual goal (a comprehensible system) with an impossible
one.

### No binary compatibility with Linux or Windows

KyuzenOS does not run Linux ELFs or Windows PE binaries. It has its own syscall
interface and its own executable format conventions. Compatibility layers are
open-ended projects that never finish; a partial one is worse than none,
because it promises software it cannot actually run.

### No 3D, OpenGL, or Vulkan

Decided explicitly in
[2D Acceleration](history/milestones/2d_accelleration.md): KyuzenOS is a
**2D-first** system. The graphics HAL, the compositor's dirty-region model, and
the software rasteriser are all built around that. There is no 3D pipeline, and
adding one is not on the roadmap.

### No USB, Wi-Fi, or full ACPI

Out of scope. Storage is ATA PIO, input is PS/2, networking is a single wired
Intel e1000. ACPI is a minimal FADT parse, enough for S5 power-off and the
reset register — not a general AML interpreter, not thermal management, not
power management.

### No bundled general-purpose web browser

`browser.elf` renders a deliberate subset of HTML and CSS through Lexbor. It is
not a browser anyone should use for the web at large, and it is not intended to
become one. See [Browser](browser/README.md) for the supported subset.

### No new implementation languages

Development stays in **C, C++, and Rust** — the languages already used here. The
kernel, drivers, filesystem and host tools are C; applications are C or C++;
Rust is available for `no_std` userspace. Adding another language would split
the toolchain and the contributor base for no gain. (The one exception is
build orchestration, which is CMake and shell.)

### Not a filesystem that is safe against power loss

KyuzenFS V4 has **no journaling**. Writes are in-place, and recovery relies on
the crash-dump region rather than on-disk transactions. An unclean shutdown can
lose or corrupt data. This is a documented limit of the design, not a bug to be
filed — see [Filesystem](filesystem/README.md).

---

## Current, concrete limitations

These are things that are *planned or possible* but not done. They are listed
so the boundary between "unfinished" and "out of scope" is visible.

| Area | Limit |
| --- | --- |
| Sockets | TCP **client** only — no `listen`/`accept`, no UDP socket API, no socket options. One global `net_lock` serialises all socket and lwIP access. |
| Networking | IPv4 only. No IPv6. |
| Filesystem | No relative paths or working directory. No recursive directory delete. No symlinks or hard links. No permission or ownership enforcement at the FS level. |
| Process model | No signals, no process groups. `fork` is not copy-on-write. |
| Tasks | `MAX_TASKS` is 16 and `MAX_WINDOWS` is 16 — both static caps. |
| Floating point | The kernel and all userspace are built `-mno-sse -msoft-float`, so `float`/`double` are avoided throughout. FPU state **is** saved and restored per task (`kernel/sched/fpu.c`), so SSE from userspace is safe, but the toolchain does not emit it. |
| Drivers | One NIC (e1000), one disk path (ATA PIO), PS/2 input. No hotplug. |
| SMP | Verified up to 16 CPUs in QEMU. No CPU hotplug. |

## What "supported" means here

Documentation labels each subsystem as `Currently supported`,
`Not currently implemented`, `Experimental`, or `Planned`. Where a document and
the code disagree, **the code is the source of truth** — please report the
discrepancy rather than assuming the document is right.

The CI pipeline (`.github/workflows/build.yml`) enforces three claims
mechanically: that a clean clone builds, that the host tests pass, and that the
resulting ISO boots to userspace. Claims outside those three are the
documentation's responsibility.

## Related Documentation

- [Roadmap](ROADMAP-OSDEV.md) — the phased plan this scope bounds
- [Architecture Overview](architecture/overview.md) — what the system is made of
- [Design Notes](design/README.md) — rationale for individual decisions
- [Documentation Index](README.md)
