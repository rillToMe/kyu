# Interrupts & Timers

This document covers the interrupt descriptor table (IDT), interrupt service
routines (ISRs), the timer sources that drive preemption, and symmetric
multiprocessing (SMP) bring-up.

## Interrupt Descriptor Table

The IDT is set up during CPU initialization (`arch/x86/idt.c`). Gates used by
the system:

| Vector | Source | Notes |
| --- | --- | --- |
| 0–31 | CPU exceptions | Faults and traps, including `#PF`, `#GP`, `#UD` |
| 32 (0x20) | PIT timer (IRQ0) | Boot-CPU scheduler tick |
| 33 (0x21) | PS/2 keyboard (IRQ1) | Keyboard events |
| 44 (0x2C) | PS/2 mouse (IRQ12) | Mouse events |
| 0xF0 | LAPIC timer | Per-CPU scheduler tick on APs |
| 0xFD | LAPIC reschedule IPI | Cross-CPU reschedule hint |
| 128 (0x80) | Syscall | `int 0x80`, DPL 3, interrupt gate |

There is no IDT gate for ATA (IRQ14); ATA uses programmed I/O (polling).

### Exception handling

CPU exceptions are routed to the panic handler, which prints a BSOD with a
full register dump and a diagnostic explanation. See
[Panic & Crash Handling](panic.md).

## Interrupt Service Routines

ISR stubs are written in assembly (`arch/x86/isr*.asm`) and share a macro
(`arch/x86/isr_macro.inc`) that saves and restores the full register set:

```text
isr stub:
    PUSHA64             push all general-purpose registers
    mov rdi, rsp        pass the frame pointer
    call <C handler>
    POPA64              restore registers
    IRETQ               return
```

This gives every interrupt the same `registers_t` frame layout the scheduler
expects.

## Timer Sources

| CPU | Timer | Vector | Purpose |
| --- | --- | --- | --- |
| BSP | PIT (IRQ0) | 32 | Scheduler tick, uptime, callbacks |
| APs | LAPIC timer | 0xF0 | Scheduler tick on application processors |

### Refresh rate

The timer runs at a configurable refresh rate. The default is **60 Hz**; the
shell can switch between presets:

```text
refresh          # show current rate
refresh 60
refresh 100
refresh 144
```

`timer_get_ms()` is based on a runtime accumulator, not a compile-time
constant, so uptime, sleeps, lwIP timeouts, and the scheduler stay consistent
when the refresh rate changes. The scheduler quantum remains a fixed **20 ms**
of wall time; the timer IRQ frequency is independent.

### Timer callbacks

`kernel/timer_callbacks.c` registers periodic work on each tick:

- Visual flush (compositor present)
- Cursor update
- Network poll (`cb_network`) — drives lwIP timeouts and e1000 RX
- Filesystem sync (approximately every 3 seconds)

Callbacks are registered and unregistered under IRQ-save locking so they cannot
race the handler.

## Per-CPU Interrupt Stacks

Each CPU has dedicated stacks so a ring transition has a safe stack:

- **Idle stack** (`TASK_STACK_SIZE` = 16 KB) — used by the idle loop.
- **Syscall stack** (`SYSCALL_STACK_SIZE` = 16 KB) — used for `int 0x80` entry
  and kernel-context tasks.

The TSS `RSP0` is repointed on every context switch to the incoming task's
kernel stack (see [Scheduler](scheduler.md)), which is required for correctness
when multiple Ring-3 tasks share a CPU.

## SMP Bring-Up

KyuzenOS supports up to 16 CPUs (`SMP_MAX_CPUS`). Bring-up proceeds as follows:

1. **BSP initialization** — GDT/TSS, IDT, SMAP/SMEP, WP, PIT timer.
2. **LAPIC detection** — discover local APICs via the ACPI/MADT or Limine
   data.
3. **AP wake-up** — send startup IPIs; each AP starts in a trampoline.
4. **Per-AP initialization** — each AP loads the GDT, installs its own TSS
   (`tss_set_rsp0` + `ltr`), loads the IDT, enables SMAP/SMEP, and programs its
   LAPIC timer.
5. **Join the scheduler** — each AP enters the idle loop and participates in
   the run-queue system.

### Per-CPU TSS

The GDT contains one TSS descriptor per CPU:

```
TSS_SELECTOR(cpu) = (GDT_TSS_FIRST + 2*cpu) << 3
```

with `GDT_TSS_FIRST = 5` and each descriptor occupying two GDT slots (16 bytes).
For 16 CPUs this yields 37 GDT entries (`5 + 2*SMP_MAX_CPUS`).

## LAPIC Vectors

| Vector | Name | Purpose |
| --- | --- | --- |
| `0xF0` | LAPIC timer | Preemption tick on APs |
| `0xFD` | Reschedule IPI | Sent to nudge a CPU to reschedule |

The reschedule IPI is a **hint only**; correctness never depends on its
delivery. A CPU that misses it still observes pending work at its next tick.

## Related Documentation

- [Scheduler](scheduler.md) — how the timer drives context switches
- [Synchronization](synchronization.md) — IRQ-safe locking
- [Panic & Crash Handling](panic.md) — exception path
- [Architecture: Boot](../architecture/boot.md) — initialization order
