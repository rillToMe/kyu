# Arch Hardening — Lapisan `arch/x86/`

This document is the reference for the hardening layer of the x86-64
architecture code: what protections exist, why each one is there, and which
invariants must not be broken. It is the counterpart to
[Interrupts & Timers](interrupts.md), which describes the mechanics; this page
describes the **defenses**.

> **Origin.** The whole layer was audited for production readiness on
> 2026-10-03. 15 findings were recorded and 12 fixed (all 5 critical blockers).
> The full record — including the four bugs found only via boot-testing — is in
> [FIX_008](../history/milestones/FIX_008_arch_hardening.md).

## CPU feature enablement

Four CPU features are enabled **per-core** (CR4 and EFER are not shared between
cores), so both the BSP and every AP must run the same sequence:

| Feature | Where | Why it matters |
| --- | --- | --- |
| `CR4.SMEP` | `kernel/cpu.c` | Kernel cannot execute user pages |
| `CR4.SMAP` | `kernel/cpu.c` | Kernel cannot access user pages without `STAC` |
| `CR0.WP` | `kernel/cpu.c` | Kernel cannot write read-only pages |
| `EFER.NXE` | `kernel/cpu.c` | Enables the NX (no-execute) PTE bit |
| `CR4.OSFXSR` + `OSXMMEXCPT`, `CR0.EM=0`/`MP=1`/`NE=1` | `kernel/sched/fpu.c` | SSE usable from ring 3 |

**Enable order matters.** `EFER.NXE` must be active before any user mapping is
created, because the ELF loader sets the NX bit — setting that bit without NXE
raises `#GP`. Similarly `CR4.OSFXSR` must be set before a ring-3 task executes
its first SSE instruction, or that task takes `#UD`.

Every enable function **verifies** the result rather than assuming success. If
`wrmsr` is ignored (some hypervisors) `g_nx_enabled` stays 0, and the NX bit is
never written into a PTE — a CPU without NX support must not receive that bit.

### Capability flags

| Flag | Meaning |
| --- | --- |
| `g_smep_enabled` | `CR4.SMEP` active on this CPU |
| `g_smap_enabled` | `CR4.SMAP` active — also gates the `clac` in the ISR prologue |
| `g_nx_enabled` | `EFER.NXE` active — also gates `PAGING_NX` in PTE flags |

`g_smap_enabled` is read by assembly (`KZ_ENTRY_PROLOGUE`). It is set **before**
CR4 is written and never changes afterward, so the branch is perfectly
predictable.

## Entry prologue — `cld` + `clac`

Every interrupt/trap stub begins with `KZ_ENTRY_PROLOGUE`
(`arch/x86/isr_macro.inc`), which does two things **before the first `push`**:

```asm
cld                     ; DF = 0
cmp byte [rel g_smap_enabled], 0
je  %%no_clac           ; CPU without SMAP would #UD on clac
clac                    ; AC = 0 → SMAP enforced
```

### Why `cld`

The x86-64 ABI requires `DF = 0` on function entry, but **the CPU does not
normalize it on interrupt/trap entry** — it only saves `RFLAGS`. `DF` is
settable from CPL3 without `IOPL`:

```asm
pushfq
or qword ptr [rsp], 0x400   ; DF = 1
popfq
```

If that leaks into the kernel, every `rep movs` runs **backwards**. The kernel
uses one such instruction in a syscall path (`kernel/gfx/kwm.c`, canvas blit via
syscall 31), so a hostile application could make the kernel write behind a
buffer and corrupt the kernel heap.

### Why `clac`

The gate for SMAP is the **AC flag**, not `CR0.AM`. `AC` is also settable from
CPL3, and while `AC = 1` SMAP is **inactive** — the kernel may freely touch
US=1 pages. That defeats the entire boundary-copy model (`include/smap.h`),
which relies on user memory being reachable only inside explicit
`user_access_begin()` / `user_access_end()` windows.

Without `clac` in the prologue, an application can disable SMAP for the duration
of any syscall simply by setting `AC` first.

### Where the prologue is applied

| Location | Count |
| --- | --- |
| `arch/x86/exceptions.asm` — `ISR_NOERRCODE` / `ISR_ERRCODE` (0..31) | 32 |
| `arch/x86/exceptions.asm` — `ISR_DEFAULT` (32..255) | 220 |
| Standalone stubs (`timer`, `lapic_timer`, `reschedule`, `isr14`, `isr128`, `keyboard`, `mouse`) | 7 |

**Not** applied to `gdt_flush.asm`, `idt_flush.asm`, `idle_switch.asm` — these
are called from C as ordinary functions, so no CPL transition can carry user
flags into them.

## IDT — all 256 vectors

Every vector has a stub. Unassigned vectors land in
`default_interrupt_handler()` (`arch/x86/vectors.c`), which classifies them:

| Class | Vectors | Action |
| --- | --- | --- |
| Spurious LAPIC | 255 | EOI only — this is a **normal** APIC event |
| PIC IRQ without a driver | 32..47 | EOI + report (rate-limited to 8) |
| CPU exception | 0..31 | Panic path |
| Anything else | 48..254 | Report + panic |

Without this, an unassigned vector resolves to a zero IDT entry → `#GP` →
`#DF` → triple fault. The spurious LAPIC vector (255) is the realistic case: the
LAPIC SVR is programmed with `0xFF`, so that vector **will** be delivered
eventually.

### Gate attributes

| Gate | DPL | Rationale |
| --- | --- | --- |
| `#BP` (3), `#OF` (4), `#BR` (5), `#UD` (6) | **3** | `int3`/`into`/`bound`/`ud2` from ring 3 must raise the real exception, not `#GP`. This is what makes user-space debugging possible. |
| `int 0x80` (128) | **3** | Syscall entry |
| Everything else | 0 | Kernel-only |

### Error-code correctness

Only the vectors listed in Intel SDM Vol.3 Table 6-1 push an error code
(`#DF`, `#TS`, `#NP`, `#SS`, `#GP`, `#PF`, `#AC`, `#CP`, `#VC`, `#SX`). Using
`ISR_ERRCODE` on a vector that does **not** push one shifts the entire frame by
8 bytes and makes `registers_t` read the wrong fields — a confusing crash.
`exceptions.asm` matches the SDM table.

## Interrupt Stack Table (IST)

`#DF`, NMI, and `#MC` each get a **dedicated stack**, because their whole point
is that they may fire when the current stack is unusable:

| Vector | IST index | Why |
| --- | --- | --- |
| `#DF` (8) | `KZ_IST_DF` | This is *the* fault that happens on a bad stack |
| NMI (2) | `KZ_IST_NMI` | Asynchronous, can hit any state |
| `#MC` (18) | `KZ_IST_MC` | Hardware fault, stack may be gone |

Without IST, a kernel stack overflow produces: `#PF` (stack overflow) → `#DF`
(no stack to push the frame) → **triple fault → reset with no BSOD, no
crashdump, no log**.

The stacks are **static BSS** (`ist_stacks[16][3][8192]`, 384 KB), not
`kmalloc`, because `init_gdt()` runs in `boot_phase_cpu_tables()` **before the
heap exists**.

## Task stacks and guard pages

Task kernel stacks are **not** allocated with `kmalloc`. The heap has no guard
page, so a stack overflow silently overwrites the next heap block — detected
only much later (or never). `kernel/sched/task_stack.c` allocates from the PMM
and leaves **one page unmapped** below the stack:

```
+------------------------+  <- base + PAGE + TASK_STACK_SIZE  (stack_top)
|   stack pages (16 KB)  |     stack grows downward from here
+------------------------+  <- base + PAGE
|  GUARD (not mapped)    |     #PF with CR2 == base
+------------------------+  <- base
```

An overflow is now a `#PF` on the **first instruction that touches it**, with
`CR2` pointing exactly at the guard.

> **Invariant:** `tasks[].stack_base` holds the **base** (the guard page
> address), and `stack_top` must be computed with `task_stack_top()`. The old
> `stack_base + TASK_STACK_SIZE` formula would point 4 KB below the real top —
> putting `RSP0` in the middle of the stack, so a ring-3→ring-0 transition would
> overwrite the task's own data.

## Ring-3 fault isolation

A fault from CPL=3 belonging to a `TASK_KIND_SPAWNED` task **kills that task**,
not the system (`kz_try_kill_faulting_user_task()`, `kernel/panic/panic.c`). One
application segfaulting must not take down the desktop.

**Still panics, deliberately:**

| Condition | Why |
| --- | --- |
| CPL=0 | A kernel fault is a kernel bug — nothing can be safely "recovered" |
| NMI / `#DF` / `#MC` | Machine conditions, not process faults |
| No user task active | Nothing to blame; do not swallow the fault |

**Why it is safe to terminate from here:** the faulting task is RUNNING on this
CPU (it cannot run elsewhere), a user-mode path holds **zero** kernel locks when
it enters ring 0 (so exit cannot self-deadlock), and `proc_exit_kill()` performs
a single authoritative transition.

## W^X — NX and ELF segment permissions

`kernel/proc/elf.c` honours `p_flags`:

| ELF flag | PTE result |
| --- | --- |
| `PF_X` | NX **cleared** (executable) |
| no `PF_X` | NX **set** (not executable) |
| `PF_W` | RW=1 |
| no `PF_W` | RW=0 |

The user **stack** is mapped `paging_user_rw_nx()` — writable, not executable.

### The map-writable → fill → reprotect pattern

With `CR0.WP=1` the kernel cannot write a page whose RW bit is 0, so a segment
cannot be mapped read-only before its contents are copied — the copy would
`#PF`. The loader therefore maps segments writable, fills them, then applies the
final flags per page via `vmm_protect_page_into()`.

Reprotection is per-page because one segment can span many PTEs, and only pages
actually belonging to that segment may be changed — a page shared with the *next*
segment must not be tightened early.

### NX is opt-out

The NX bit means "**not** executable", so an executable segment must have it
**cleared**. `paging_nx_flag()` returns 0 when `EFER.NXE` is unavailable, which
keeps the flag out of PTEs entirely on CPUs without NX.

## FPU / SSE state per task

`kernel/sched/fpu.c` saves and restores x87 + XMM state with
`FXSAVE64`/`FXRSTOR64` on every context switch. Without it, XMM registers are
**global per-CPU state**: the next task sees the previous task's leftover
values. That is a cross-process data leak (as serious as reading another
process's memory, and invisible to the page tables) plus randomly wrong
computation results depending on scheduling order.

`fpu_state[512]` lives in the TCB, **16-byte aligned** — `FXSAVE` raises `#GP`
on a misaligned address.

A task that has never been saved gets a **clean** state: `fninit` for x87, and
zeroed area + default MXCSR (`0x1F80`) + default FCW (`0x037F`) restored via
`FXRSTOR`. XMM has no "reset" instruction, so this is how leftover values are
cleared.

> **The kernel/userspace asymmetry.** Since Phase 3 the two domains have
> deliberately **opposite** FP policies:
>
> - **Userspace may use float/double.** `fpu_switch()` runs between tasks, so
>   XMM is preserved. The userspace build no longer carries `-mno-sse`, and
>   `difftime` (returns `double`) is active in the C SDK.
> - **The kernel may not.** Interrupt entry does **not** save XMM:
>   `arch/x86/isr_macro.inc` pushes the general-purpose registers only
>   (`PUSHA64`), and `fpu_switch()` runs only on a context switch. Kernel code
>   using SSE would therefore clobber a running ring-3 task's XMM registers
>   every time an interrupt landed inside it, and the task would resume with
>   wrong FP values — silently, and depending on where the timer fired.
>
> This is not theoretical: `kernel/string.c` compiled without `-mno-sse` emits
> `movups %xmm0` inside `memcpy`, and `memcpy` is reachable from scheduler and
> filesystem paths that run inside interrupts.
>
> The ban is enforced mechanically, not by comment:
> `cmake/verify_kernel_elf.cmake` **fails the build** if the kernel image
> contains SSE/x87. The userspace side is checked by
> `cmake/verify_user_elf.cmake`, which **allows** xmm/x87 and rejects only
> `ymm`/`zmm` (see below).

**FXSAVE vs XSAVE:** FXSAVE covers every register SSE/SSE2 can touch, which is
the maximum the kernel promises userspace. XSAVE needs a CPUID probe, a dynamic
area size (thousands of bytes with AVX-512), and XCR0 handling - not worth it for
SSE-only coverage. `fpu.c` is the right place to switch if AVX is added later.

**Why AVX is rejected in user ELFs today:** `FXSAVE` does not save the upper
128 bits of YMM/ZMM — that needs `XSAVE`. An AVX instruction in a user ELF would
have its upper halves silently clobbered by the next task. The default `-march`
for this target is baseline x86-64 (SSE2, no AVX), so nothing emits `ymm` today;
the check in `verify_user_elf.cmake` keeps it that way if a `-march` is ever
raised or a vendored library ships precompiled AVX code.

## Frame layout invariants

`registers_t` (`include/task.h`) is the mirror of the frame `PUSHA64` builds.
**23 `_Static_assert`s** pin the size (176 bytes) and the offset of every field.

> This contract is guarded by the compiler, not by a comment. When the asserts
> were written they immediately caught a miscount by their own author
> (`184 == 176` failed — 22 fields × 8 bytes, not 23).

### Two frame shapes

The CPU pushes a **different number of qwords** depending on the CPL at the time
of the interrupt:

| Interrupted from | Qwords pushed | Fields |
| --- | --- | --- |
| Ring 3 (user) | **5** | SS, RSP, RFLAGS, CS, RIP |
| Ring 0 (kernel) | **3** | RFLAGS, CS, RIP |

A stack switch only happens from ring 3, so only there are SS:RSP pushed. This
has two consequences:

1. **Alignment differs.** A ring-3 frame lands at `RSP0` (16-aligned), so
   `registers_t` ≡ 0 (mod 16). A ring-0 frame sits where the interrupted kernel
   code was — inside a function body, ≡ 8 (mod 16) because `call` already pushed
   a return address. Any validation of a handler-returned RSP must accept
   **8-byte** alignment, not 16.
2. **The last two fields are unreliable for ring-0 frames.** `registers_t.rsp`
   and `.ss` read memory *above* the frame. This is harmless for dumps and does
   not affect context switching, because `iretq` also pops only 3 qwords when
   returning to the same CPL — pushes and pops stay consistent.

### Fake frames for new tasks

`kernel/sched/lifecycle.c` forges a frame so a task can be resumed through the
normal `POPA64` + `IRETQ` path. Because `iretq` does **not** push a return
address, `RSP` at task entry would land ≡ 0 (mod 16) — but the SysV ABI requires
≡ 8 on function entry. `KZ_FAKE_FRAME_PAD` (8 bytes) is inserted **above** the
frame, so `registers_t` stays exactly 176 bytes and no field offset shifts; only
the final RSP moves.

## RSP validation before context switch

The three stubs that switch stacks (`timer`, `lapic_timer`, `reschedule`) take
the next task's RSP from a C handler's return value and load it directly:

```asm
call handler
mov  rsp, rax      ; RSP from C, used immediately
POPA64
iretq
```

If the handler returns something unreasonable, the kernel crashes **far from the
cause** — usually `#DF` or triple fault, because the stack is no longer usable to
report anything. `KZ_VALIDATE_RSP` (`arch/x86/isr_macro.inc`) checks the value
first:

| Check | Rejects |
| --- | --- |
| Canonical address | Non-canonical pointers |
| Higher-half | User addresses (a handler returning a user pointer would run the kernel on an application's stack) |
| 8-byte aligned | Misaligned frames |
| Not at the very end of address space | Wraparound |

On failure it reports the value to COM1 and falls back to the current ISR frame's
RSP — already valid, because the ISR just ran on it — so the task continues
instead of crashing mysteriously.

## Kernel heap and kernel stack separation

Kernel task stacks are **not** heap objects (see [guard pages](#task-stacks-and-guard-pages)).
Allocating them from `kmalloc` and then unmapping the guard page would be wrong:
the page would still belong to a heap block, so a later `kfree` would write a
block header there — a `#PF` on the free path, or worse, the heap allocator
walking a list with a missing header.

Kernel stacks come from the PMM and are mapped manually. This matches Linux
(`alloc_thread_stack_node`): a kernel stack is not a heap object.

## Per-CPU GS base

`smp_current_cpu_index()` uses a **single load from `GS:0`** instead of reading
uncached LAPIC MMIO and scanning a 16-entry table. It runs in every ISR and in
dozens of other paths, so the old version paid a real cost at high tick rates —
and depended on MMIO that may not respond precisely when a CPU is in trouble
(the panic path).

`percpu_t.cpu_index` is the **first field** (offset 0); `smp_set_gs_base()` is
called for the BSP in `smp_init()` and for every AP in `smp_ap_main()` **before**
any code that can call `smp_current_cpu_index()`. If GS is not yet set (very
early boot) the function falls back to the old LAPIC scan, so it stays correct
throughout the boot sequence rather than assuming GS is always ready.

**Why GS and not FS:** KyuzenOS userspace does not use TLS (static ELF, no
`PT_TLS`), so FS is left free for that future need.

## LAPIC timer calibration

The LAPIC timer counts at bus clock / divider, which **differs between
machines**. A hardcoded `INIT_COUNT` therefore produces an unknown tick rate —
the old constant `1000000` yielded ~12 Hz on the test host, not the intended
100 Hz. `arch/x86/lapic.c` now calibrates against the **PIT channel 0** at boot.

**Why channel 0 and not channel 2** (which the kernel does not use): an earlier
attempt with channel 2 produced two measurements that disagreed by **9×** with no
way to tell which was right. Channel 0 removes that whole class of problem — it
is already programmed by `init_timer()` for 60 Hz, its rate is **already proven
correct** (the entire OS timebase depends on it), and the calibration only
**reads** it (reprogramming would corrupt the system timebase).

> **Boot order invariant:** `init_timer()` must run **before** `lapic_init_bsp()`.
> The original order calibrated against leftover firmware values, giving an 18%
> spread between boots.

`lapic_timer_ticks()` is an **aggregate** counter — every CPU adds its own ticks,
including the BSP. So the aggregate rate is (number of CPUs running the LAPIC
timer) × target Hz.

## String operations

`kernel/string.c` uses `rep movsb` / `rep stosb` for blocks **≥ 128 bytes**; below
that it keeps the byte loop that clang unrolls. The threshold matters: `rep movsb`
has ~30-40 cycles of setup cost and would slow down the small copies that make up
most call sites.

**`cld` is written explicitly** before every string instruction. Relying on "the
entry stub already did `cld`" is exactly the hidden assumption that caused the
original DF bug. The backward path (`memmove` with overlap) uses
`std; rep movsb; cld` — the trailing `cld` matters because leaving `DF = 1` would
corrupt every subsequent string instruction in that thread.

## Known limitations

| Limitation | Notes |
| --- | --- |
| No exception fixup table | A fault inside `copy_from_user` is **deterministic and visible** (ring 3 → task killed; ring 0 → readable BSOD), not silent corruption. What is missing is turning it into a handleable `-EFAULT`. Plan in [FIX_008](../history/milestones/FIX_008_arch_hardening.md#arch-7--exception-fixup-table-dan-fixup_bad_iret). |
| No `fixup_bad_iret` | A corrupt frame causes `iretq` to fault. Same fault path as above; deferred for the same reason. |
| No KPTI | The kernel stays mapped while ring 3 runs. Only relevant on CPUs without `RDCL_NO`. |
| No CI | Host tests exist (`./build.sh test`) but nothing runs them automatically. |

## Related documentation

- [Interrupts & Timers](interrupts.md) — IDT, ISR mechanics, timer sources
- [Scheduler](scheduler.md) — context switch, per-task stacks
- [Panic & Crash Handling](panic.md) — fault reporting, kill path
- [Arch Subsystem Map](../design/arch-subsystem-map.md) — which subsystems exist,
  which are missing, and what is deliberately out of scope
- [FIX_008](../history/milestones/FIX_008_arch_hardening.md) — the audit and fix record
