# Debugging

KyuzenOS provides several mechanisms for diagnosing kernel and userspace
problems: serial logging, the BSOD, crash reports, and hardware watchpoints.

## Serial Logging

COM1 is always directed to `serial.log` by the `run` targets. A panic therefore
leaves a trace even when the display is unreadable.

```sh
make run            # serial → serial.log
make run-serial     # serial → stdout
make run-wd         # serial → serial.log (4 CPUs)
```

The kernel emits `[P1]`..`[P4]` markers along the panic path so a truncated log
still shows how far execution reached. Serial output in panic mode is bounded
(no unbounded blocking writes).

## BSOD (Blue Screen of Death)

An unrecoverable fault enters panic mode and displays the BSOD, which includes:

- The exception vector and mnemonic.
- A full general-purpose register dump.
- `CR2` (faulting address) for page faults.
- The current task and CPU context.
- The last syscall number and arguments (best effort).
- A human-readable explanation of the fault.

The BSOD waits for operator input: `R` to reboot, `S` to shut down. There is no
automatic reboot. See [Panic & Crash Handling](../kernel/panic.md).

## Crash Report and Crash Dump

- `/crash-report.txt` is published for the next boot and is readable from the
  File Manager. The desktop shows a notification card for a fresh crash.
- A crash dump is written to the last 8 sectors of the disk (a reserved region
  the filesystem never allocates). A host-side `--dump` decoder can read it.

The crash dump uses best-effort writes so the panic path never blocks on a held
lock.

## Watchpoints

For memory-corruption hunting, the kernel supports per-CPU DR0 hardware
watchpoints with a log-and-continue `#DB` handler. This lets you trace which
code writes a given address without stopping the system.

The heap canary builds (`make heap-stress`, `make heap-watch`) detect heap
overflow and corruption.

> Note: the `heap-stress`/`heap-watch` targets are currently affected by the
> stale source-list issue described in [Testing](testing.md).

## Scheduler Diagnostics

The shell `sched` command (`scheduler_dump`) prints the active task, online
CPUs, the CPU→task map, and each CPU's run-queue length (`cpuN=...(q=M)`).

## Reusable Debugging Techniques

These techniques were used to diagnose real defects and are worth knowing:

- **Per-CPU DR0 watchpoints** with a log-and-continue handler to catch the
  writer of a corrupted address.
- **PIT channel 2 / PS-2 polling** to keep input alive when interrupts are
  suspect.
- **Page-walk inspection** — walking the page tables to confirm what a virtual
  address actually maps to (used to prove a heap page was backed by BIOS ROM).
- **Raw evidence over conclusions** — keep the log fragments, addresses, and
  disassembly, not just the final diagnosis.

The [heap corruption post-mortem](../troubleshooting/2026-07-26-heap-corruption-bosd.md)
is a worked example that applied all of these.

## IntelliSense

For accurate IDE diagnostics, generate the compilation database:

```sh
make compile_commands
```

See [Building](building.md) for setup.

## Related Documentation

- [Panic & Crash Handling](../kernel/panic.md)
- [Testing](testing.md)
- [Running](running.md)
- [Heap corruption post-mortem](../troubleshooting/2026-07-26-heap-corruption-bosd.md)
