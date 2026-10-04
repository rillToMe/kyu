# Panic & Crash Handling

When the kernel detects an unrecoverable fault, it enters **panic mode** and
displays a Blue Screen of Death (BSOD). This document describes the panic
path, the diagnostics it produces, and how crash information survives to the
next boot.

## Overview

The panic subsystem lives in `kernel/panic/` (orchestrator, draw, hardware,
explain) with supporting code in `kernel/debug/` (BSOD persistence, crash dump,
RAM log).

| Concept | Description |
| --- | --- |
| Panic mode | A locked state entered on an unrecoverable fault |
| BSOD | The on-screen diagnostic display |
| Crash report | `/crash-report.txt`, written for the next boot to read |
| Crash dump | A snapshot written to reserved disk sectors |
| Crash notification | A desktop card shown on the next boot |

## Entering Panic Mode

The panic path is triggered by CPU exceptions (routed from the IDT) and by
explicit `panic()` calls. Once entered:

- The panic state is **locked**: no further panics may nest.
- A nested-panic guard prevents recursive panics from corrupting the display.
- The compositor stops and hides the cursor (`compositor_panic_cursor_off`).

### Ring-3 faults do not panic

A fault from **CPL=3** belonging to a `TASK_KIND_SPAWNED` task **kills that task
only**. `kz_try_kill_faulting_user_task()` runs at the very top of
`exception_handler()` — before any panic state is touched — and calls
`proc_exit_kill()`. The kernel, the desktop, and every other task keep running.

This is checked **first**, so one application segfaulting cannot take down the
system. The event is still recorded on COM1 so a crashing application can be
diagnosed without reproducing it under a debugger:

```text
[FAULT] task 7 "browser" vektor=14 err=0x6 rip=0x0000000004001A2F -> task dibunuh, kernel lanjut
```

**Still panics, deliberately:**

| Condition | Why |
| --- | --- |
| CPL=0 | A kernel fault is a kernel bug; nothing can be safely recovered |
| NMI (2), `#DF` (8), `#MC` (18) | Machine conditions, not process faults |
| No user task active | Nothing to blame; the fault must not be swallowed |

This is safe because the faulting task is RUNNING on this CPU (it cannot run
elsewhere), a user-mode path holds **zero** kernel locks when it enters ring 0
(so the exit path cannot self-deadlock), and `proc_exit_kill()` performs a single
authoritative transition. See [Arch Hardening](arch-hardening.md).

### IST — faults that need their own stack

`#DF`, NMI, and `#MC` run on dedicated IST stacks, because they may fire when
the current stack is already unusable. Without IST, a kernel stack overflow
produces `#PF` (overflow) → `#DF` (no stack for the frame) → **triple fault:
reset with no BSOD, no crashdump, no log**. Task stacks additionally have a
guard page, so an overflow faults on the first instruction that touches it with
`CR2` pointing exactly at the guard.

### No-interrupt panic path

In panic mode, the system must still be able to respond to keyboard input
(`R` to reboot, `S` to shut down) without relying on the normal interrupt
machinery, which may be the very thing that failed. The panic path therefore
polls hardware directly:

- **PIT channel 2** as a rate generator (~100 Hz) — programmed with `0xB6`
  (channel 2, lobyte/hibyte, mode 3), with the gate enabled via port `0x61`.
  Channel 2 is used because channel 0 is the IRQ0 source and the panic path has
  already executed `cli`; channel 2 produces no IRQ and its output level can be
  polled from port `0x61`. Time is derived from level transitions
  (`PIT2_MS_PER_EDGE` = 5 ms).
- **PS/2 keyboard** polled directly for the operator's keypress.
- Direct framebuffer access via the scanout surface.

Panic mode performs **no auto-reboot** by design; it waits for the operator.

> **Note on channel 2 sharing.** The panic path programs PIT channel 2 as a
> rate generator and enables its gate via port `0x61`. LAPIC calibration must
> therefore **not** use channel 2 — an early revision did, and two different
> measurement approaches disagreed by 9× with no way to tell which was right.
> Calibration reads **channel 0** instead (see
> [Arch Hardening](arch-hardening.md)). The two do not conflict in practice:
> calibration runs once at boot, and the panic path reprograms channel 2 only
> after a fault.

## BSOD Display

The BSOD shows:

- The exception vector and mnemonic
- A full register dump (all general-purpose registers)
- `CR2` (the faulting address for page faults)
- The current task and CPU context
- The last syscall number and arguments (best-effort diagnostics)
- A human-readable explanation of the fault

The explanation layer (`panic_explain.c`) maps vectors to descriptions.

## Crash Persistence

Crash information is preserved across a reboot through two mechanisms.

### Crash report file

A `/crash-report.txt` file is published to the filesystem. On the next boot it
is readable from the File Manager. The desktop shows a notification card when
a fresh crash is detected.

### Crash dump

A crash dump is written to the **last 8 sectors** of the disk
(`KZFS_CRASHDUMP_SECTORS`), a region the filesystem never allocates. The dump
records:

| Field | Value |
| --- | --- |
| Magic | `CRASHDUMP_MAGIC` = `0x44435A4B` ("KZCD") |
| Version | `CRASHDUMP_VERSION` = 1 |
| Max size | `CRASHDUMP_MAX_BYTES` = 4096 |
| Truncation flag | `CRASHDUMP_FLAG_TRUNCATED` = 1 |

The crash-dump write is best-effort: `kfs_sync_all_try` and
`bcache_flush_all_try` are used so the panic path never blocks on a lock held
by the failed CPU.

> **Ordering rule:** the screen must be drawn **before** persistence is
> attempted, so a hang during the disk write still leaves diagnostics visible.

## Crash Notification

On boot, the kernel probes for a fresh crash and exposes it through syscall 80
(`SYS_CRASH_NOTICE`). The `crash_notice_t` structure is:

```c
typedef struct {
    uint32_t pending;
    uint32_t crash_count;
    uint64_t uptime_ms;
    uint64_t vector;
    uint64_t rip;
    uint64_t cr2;
    uint32_t task_id;
    char     task_name[16];
    char     path[32];
} crash_notice_t;
```

The desktop's `crash_notice` module polls it once at startup, shows a card for
`NOTIF_MS`, and opens the File Manager when the card is clicked. The archive
path is `CRASH_ARCHIVE_PATH` = `/crash-report.txt`.

## Debugging Facilities

The panic subsystem also provides tools used during normal development:

- **RAM log** — an in-memory log buffer that survives into the BSOD display.
- **Serial markers** — bounded `[P1]..[P4]` markers on COM1 that trace the
  panic path even when the display is unavailable.
- **`--dump` decoder** — a host-side tool to decode a crash dump.
- **Watchpoints** — per-CPU DR0 watchpoints with a log-and-continue `#DB`
  handler, used to trace memory corruption.

These are described further in [Debugging](../development/debugging.md).

## Design Constraints

The panic path is deliberately conservative:

- **No allocation.** The panic path must not depend on the heap, which may be
  corrupt.
- **No locking that can block.** Only try-locks are used.
- **Never read user memory.** A fault may be caused by a bad user pointer;
  the panic path does not dereference user addresses.
- **Bootloader pointers are already virtual.** Limine framebuffer pointers are
  virtual addresses, not physical; the panic path treats them as such.
- **PS/2 must consume mouse bytes.** The keyboard poll must drain pending mouse
  bytes so the input stream does not desynchronize.

## Related Documentation

- [Interrupts & Timers](interrupts.md) — exception vectors
- [Debugging](../development/debugging.md) — serial logging and watchpoints
- [Heap corruption post-mortem](../troubleshooting/2026-07-26-heap-corruption-bosd.md) — a worked example
- [Filesystem](../filesystem/README.md) — crash-dump region and sync
