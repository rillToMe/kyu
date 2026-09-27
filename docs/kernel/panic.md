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

### No-interrupt panic path

In panic mode, the system must still be able to respond to keyboard input
(`R` to reboot, `S` to shut down) without relying on the normal interrupt
machinery, which may be the very thing that failed. The panic path therefore
polls hardware directly:

- PIT channel 2 (PS/2) polling for input
- Direct framebuffer access via the scanout surface

Panic mode performs **no auto-reboot** by design; it waits for the operator.

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
