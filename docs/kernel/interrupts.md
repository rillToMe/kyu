# Interrupts & Timers

This document covers the interrupt descriptor table (IDT), interrupt service
routines (ISRs), the timer sources that drive preemption, and symmetric
multiprocessing (SMP) bring-up.

## Interrupt Descriptor Table

The IDT is set up during CPU initialization (`arch/x86/idt.c`). **All 256 vectors
have a stub** — a vector without a gate resolves to a zero IDT entry, which
raises `#GP` → `#DF` → triple fault. Vectors without a dedicated handler land in
`default_interrupt_handler()` (`arch/x86/vectors.c`).

| Vector | Source | Notes |
| --- | --- | --- |
| 0–31 | CPU exceptions | Faults and traps, including `#PF`, `#GP`, `#UD` |
| 32 (0x20) | PIT timer (IRQ0) | Boot-CPU scheduler tick |
| 33 (0x21) | PS/2 keyboard (IRQ1) | Keyboard events |
| 44 (0x2C) | PS/2 mouse (IRQ12) | Mouse events |
| 0xF0 | LAPIC timer | Per-CPU scheduler tick on APs |
| 0xFD | LAPIC reschedule IPI | Cross-CPU reschedule hint |
| 0xFF | LAPIC spurious | EOI only — this is a normal APIC event |
| 128 (0x80) | Syscall | `int 0x80`, DPL 3, interrupt gate |

There is no IDT gate for ATA (IRQ14); ATA uses programmed I/O (polling).

### Gate privilege levels

| Gate | DPL | Rationale |
| --- | --- | --- |
| `#BP` (3), `#OF` (4), `#BR` (5), `#UD` (6) | **3** | `int3`/`into`/`bound`/`ud2` from ring 3 must raise the real exception, not `#GP`. This is what makes user-space debugging possible. |
| Syscall (128) | **3** | Ring-3 entry point |
| All others | 0 | Kernel-only |

### Interrupt Stack Table (IST)

`#DF`, NMI, and `#MC` run on dedicated stacks, because they may fire when the
current stack is already unusable — a `#PF` from stack overflow followed by `#DF`
on the same stack is a triple fault with no BSOD. Without IST that failure mode
is silent.

| Vector | IST | Why |
| --- | --- | --- |
| `#DF` (8) | `KZ_IST_DF` | The fault that happens *on* a bad stack |
| NMI (2) | `KZ_IST_NMI` | Asynchronous, can hit any state |
| `#MC` (18) | `KZ_IST_MC` | Hardware fault, stack may be gone |

IST stacks are static BSS (8 KB × 3 × 16 CPUs) rather than `kmalloc`, because
`init_gdt()` runs before the heap exists. Details in
[Arch Hardening](arch-hardening.md).

### Exception handling

CPU exceptions are routed to the panic handler, which prints a BSOD with a
full register dump and a diagnostic explanation. See
[Panic & Crash Handling](panic.md).

**Exceptions from ring 3 are isolated.** A fault in a `TASK_KIND_SPAWNED` task
kills **that task**, not the system — see [Arch Hardening](arch-hardening.md).

## Interrupt Service Routines

ISR stubs are written in assembly (`arch/x86/isr*.asm`) and share macros
(`arch/x86/isr_macro.inc`) that save and restore the full register set:

```text
isr stub:
    KZ_ENTRY_PROLOGUE   cld + clac  (normalize RFLAGS)
    push error_code     (0 when the CPU does not push one)
    push int_num
    PUSHA64             push all general-purpose registers
    mov rdi, rsp        pass the frame pointer
    call <C handler>
    POPA64              restore registers
    IRETQ               return
```

This gives every interrupt the same `registers_t` frame layout the scheduler
expects.

### Entry prologue

`KZ_ENTRY_PROLOGUE` runs **before the first `push`** and does two things:

- **`cld`** — the CPU does **not** normalize `DF` on interrupt entry, and `DF` is
  settable from CPL3 without `IOPL`. A leaked `DF = 1` makes every `rep movs` in
  the kernel run backwards, corrupting the kernel heap (the canvas blit in
  `kernel/gfx/kwm.c` uses one).
- **`clac`** — the SMAP gate is the `AC` flag, **not** `CR0.AM`. `AC` is also
  settable from CPL3, and while `AC = 1` SMAP is **inactive**, so the kernel may
  freely touch user pages — defeating the entire boundary-copy model.

`clac` is gated on `g_smap_enabled` because the instruction raises `#UD` on CPUs
without SMAP. See [Arch Hardening](arch-hardening.md) for the full rationale.

### Two frame shapes

The CPU pushes a different number of qwords depending on the CPL it interrupted:

| Interrupted from | Qwords | Fields |
| --- | --- | --- |
| Ring 3 (user) | **5** | SS, RSP, RFLAGS, CS, RIP |
| Ring 0 (kernel) | **3** | RFLAGS, CS, RIP |

A stack switch only happens from ring 3, so only there are SS:RSP pushed. This
means a ring-3 frame is 16-byte aligned while a ring-0 frame is 8-byte aligned —
relevant for any validation of a handler-returned `RSP`, and for the forged
frames used to start new tasks.

### Error-code correctness

Only the vectors in Intel SDM Vol.3 Table 6-1 push an error code (`#DF`, `#TS`,
`#NP`, `#SS`, `#GP`, `#PF`, `#AC`, `#CP`, `#VC`, `#SX`). Using the error-code
macro on any other vector shifts the frame by 8 bytes and makes `registers_t`
read the wrong fields.

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

### LAPIC timer calibration

The LAPIC timer counts at bus clock / divider, which **differs between
machines** — a hardcoded `INIT_COUNT` therefore yields an unknown tick rate. The
calibration measures the LAPIC against **PIT channel 0** during boot and derives
`INIT_COUNT` for the target 100 Hz.

`init_timer()` **must** run before `lapic_init_bsp()`: the original order
calibrated against leftover firmware values and produced an 18% spread between
boots. See [Arch Hardening](arch-hardening.md) for the four bugs found while
implementing this.

> `lapic_timer_ticks()` is an **aggregate** counter — every CPU adds its own
> ticks, including the BSP. The aggregate rate is therefore
> (number of CPUs running the LAPIC timer) × 100 Hz, not 100 Hz.

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
  from ring 3 (the TSS `RSP0` target).
- **IST stacks** (8 KB × 3) — for `#DF`, NMI, and `#MC`. See
  [IST](#interrupt-stack-table-ist) above.

The TSS `RSP0` is repointed on every context switch to the incoming task's
kernel stack (see [Scheduler](scheduler.md)), which is required for correctness
when multiple Ring-3 tasks share a CPU.

> **Invariant:** compute the stack top with `task_stack_top()`, **never**
> `stack_base + TASK_STACK_SIZE`. Task stacks have a guard page below them
> (see [Arch Hardening](arch-hardening.md)), so the old formula points 4 KB below
> the real top and would place `RSP0` in the middle of the stack.

## SMP Bring-Up

KyuzenOS supports up to 16 CPUs (`SMP_MAX_CPUS`). Bring-up proceeds as follows:

1. **BSP initialization** — GDT/TSS (incl. IST), IDT, SMAP/SMEP, WP, NX, SSE,
   PIT timer, then LAPIC calibration.
2. **LAPIC detection** — discover local APICs via the ACPI/MADT or Limine
   data.
3. **AP wake-up** — send startup IPIs; each AP starts in a trampoline.
4. **Per-AP initialization** — each AP loads the GDT, **sets its own GS base**,
   installs its own TSS (`tss_set_rsp0` + `ltr`), loads the IDT, enables
   SMAP/SMEP/WP/NX/SSE, and programs its LAPIC timer.
5. **Join the scheduler** — each AP enters the idle loop and participates in
   the run-queue system.

Every feature in step 4 is **per-core**: `CR4`, `EFER`, `GS base`, and `TR` are
not shared between cores, so an AP that skips any of them runs with weaker
protection than the BSP.

### Per-CPU TSS

The GDT contains one TSS descriptor per CPU:

```
TSS_SELECTOR(cpu) = (GDT_TSS_FIRST + 2*cpu) << 3
```

with `GDT_TSS_FIRST = 5` and each descriptor occupying two GDT slots (16 bytes).
For 16 CPUs this yields 37 GDT entries (`5 + 2*SMP_MAX_CPUS`).

### Per-CPU GS base

`smp_current_cpu_index()` uses a single load from `GS:0` instead of reading
uncached LAPIC MMIO and scanning a 16-entry table — it runs in every ISR. Each
CPU sets its own GS base during bring-up; if GS is not yet set (very early boot)
the function falls back to the LAPIC scan. Details in
[Arch Hardening](arch-hardening.md).

## LAPIC Vectors

| Vector | Name | Purpose |
| --- | --- | --- |
| `0xF0` | LAPIC timer | Preemption tick on APs |
| `0xFD` | Reschedule IPI | Sent to nudge a CPU to reschedule |
| `0xFF` | Spurious | EOI only — delivered normally by the APIC |

The spurious vector is programmed into the LAPIC SVR (`LAPIC_SVR_ENABLE | 0xFF`)
and **will** be delivered eventually, so it needs a real handler. It is handled
by `default_interrupt_handler()`, which performs the EOI and returns without
panicking.

The reschedule IPI is a **hint only**; correctness never depends on its
delivery. A CPU that misses it still observes pending work at its next tick.

## Related Documentation

- [Arch Hardening](arch-hardening.md) — IST, entry prologue, W^X, FPU state,
  guard pages, RSP validation
- [Scheduler](scheduler.md) — how the timer drives context switches
- [Synchronization](synchronization.md) — IRQ-safe locking
- [Panic & Crash Handling](panic.md) — exception path, ring-3 fault isolation
- [Architecture: Boot](../architecture/boot.md) — initialization order
- [FIX_008](../history/milestones/FIX_008_arch_hardening.md) — the audit and fix
  record for this layer
