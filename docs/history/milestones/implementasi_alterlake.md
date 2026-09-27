# KyuzenOS — Intel Alder Lake Gen12 2D GPU Acceleration

## Context

KyuzenOS sudah menyelesaikan GPU 2D roadmap utama Phase 0–25.

Baseline saat ini sudah frozen dan committed:

* GHAL architecture
* software backend
* VirtIO backend
* Intel backend
* PCI detection
* Intel GPU memory/buffer pool
* GTT
* legacy BCS ring
* command builder
* COPY/FILL/BLIT
* rectangle validation
* fences
* bounded sleep-wait
* robustness/fail-closed
* SMP-safe serialized submission
* API freeze
* GPU documentation
* benchmark infrastructure

Legacy Intel submission path targets Gen8–Gen11.

The target hardware for this extension is **Intel Alder Lake / Gen12 integrated graphics**.

The user's physical GPU device ID is NOT known yet. Do not assume one.

This is a post-freeze extension. Do not rewrite the original architecture.

---

# Absolute Rules

## 1. Preserve Phase 0–25

Do NOT redesign or regress:

* GHAL public API
* KWM
* compositor
* damage system
* software backend
* VirtIO backend
* existing Intel Gen8–11 path
* existing validation
* existing robustness tests
* existing benchmark framework

If a new abstraction is genuinely required, extend internally and minimally.

Do not break frozen APIs unless absolutely unavoidable. If unavoidable, stop and document why before changing them.

---

## 2. No guessed hardware information

Never assume:

* PCI device ID
* revision ID
* BAR layout
* GT topology
* engine availability
* register addresses
* register bit definitions
* context layout
* submission mechanism
* firmware requirements

Every hardware-specific value must come from:

1. authoritative Intel documentation,
2. existing trusted implementation/documentation,
3. or runtime PCI/MMIO discovery.

If uncertain, mark it:

`UNKNOWN`

or:

`BLOCKED`

Do NOT invent a value.

---

## 3. No fake PASS

Hardware tests may only report:

```text
PASS
FAIL
SKIP
BLOCKED
```

Use:

* PASS = actual hardware execution succeeded
* FAIL = hardware was available and test failed
* SKIP = hardware path is unavailable/not applicable
* BLOCKED = implementation or required hardware capability is not available yet

Never convert SKIP/BLOCKED into PASS.

---

## 4. Preserve fallback

Any Gen12 failure must remain non-fatal:

```text
Gen12 init failure
        ↓
Intel acceleration unavailable
        ↓
software backend
```

Never allow GPU initialization failure to panic the kernel.

Submission failure must be recoverable.

Timeouts must be bounded.

Invalid buffers/commands must be rejected.

---

## 5. No unnecessary features

This extension is for **2D acceleration only**.

Do NOT implement:

* OpenGL
* Vulkan
* Mesa
* shaders
* compute
* 3D rendering
* video acceleration
* ray tracing
* display KMS replacement
* multi-GPU
* full Linux DRM/i915 clone

Required workload:

```text
COPY
FILL
BLIT
```

Nothing more is necessary.

---

# Directory Rules

Keep Intel-specific implementation under:

```text
graphics/backend/
```

Do not create a new top-level GPU subsystem.

Potential files:

```text
graphics/backend/intel/intel_gen12.c
graphics/backend/intel/intel_gen12.h
graphics/backend/intel/intel_gen12_regs.h
graphics/backend/intel/intel_gen12_mmio.c
graphics/backend/intel/intel_gen12_mmio.h
graphics/backend/intel/intel_gen12_engine.c
graphics/backend/intel/intel_gen12_engine.h
graphics/backend/intel/intel_gen12_context.c
graphics/backend/intel/intel_gen12_context.h
graphics/backend/intel/intel_gen12_submit.c
graphics/backend/intel/intel_gen12_submit.h
graphics/backend/intel/intel_gen12_fence.c
graphics/backend/intel/intel_gen12_fence.h
```

Only create files when justified.

Avoid splitting tiny functions into excessive files.

---

# AL-1 — Alder Lake Hardware Discovery

## Goal

Determine exactly what Intel GPU exists in the physical machine.

Extend Intel diagnostics to report:

```text
Intel GPU:
  vendor: 8086
  device: XXXX
  revision: XX
  class: 030000
  BAR0: 0x........
  BAR0 size: ...
  generation: Gen12
```

Also report any information safely discoverable from PCI configuration.

Do not hardcode the user's GPU ID.

## Requirements

* Read PCI vendor/device/revision.
* Read BARs.
* Validate BARs.
* Detect Intel VGA-compatible device.
* Determine generation using a documented method.
* Preserve current Gen8–11 detection.
* Unknown generation must fail closed.

## Validation

QEMU without Intel iGPU should remain:

```text
Intel backend: unavailable
software fallback
```

Physical Alder Lake should produce a real PCI diagnostic.

## Deliverable

Hardware discovery only.

Do not implement submission yet.

---

# AL-2 — Gen12 MMIO Foundation

## Goal

Safely establish MMIO access to the actual Alder Lake GT.

Architecture:

```text
PCI BAR
  ↓
physical MMIO address
  ↓
HHDM
  ↓
volatile MMIO access
```

Implement only registers that are verified for the actual target.

## Requirements

* Validate BAR.
* Map/access through HHDM.
* Provide read32/write32 wrappers.
* Add optional readback verification.
* Never dereference invalid MMIO.
* Keep MMIO access serialized where required.
* Do not expose Intel registers through GHAL.

## Diagnostics

Example:

```text
Gen12 MMIO:
  BAR0: 0x........
  MMIO: available
  probe: PASS
```

If unsafe/unavailable:

```text
MMIO: unavailable
Gen12 acceleration: disabled
```

## No submission

Do not start engines yet.

---

# AL-3 — GT / Engine Discovery

## Goal

Determine which GPU engines are available.

The first required engine is the **blitter/copy engine**.

Discover:

```text
BCS0
```

or the appropriate Gen12 copy engine exposed by the actual hardware.

Do not assume engine numbering.

## Requirements

* Determine engine presence.
* Determine engine capabilities.
* Determine engine identity.
* Record discovery result.
* Avoid enabling an engine yet.

Example:

```text
Gen12 engines:
  copy: present
  render: ignored
  video: ignored
```

Only the copy engine is required.

## Validation

If the copy engine cannot be identified safely:

```text
Gen12 2D acceleration: BLOCKED
```

and fallback to software.

---

# AL-4 — Gen12 GPU Virtual Memory

## Goal

Determine and implement the minimum address-space mechanism required by Gen12 submission.

Existing system:

```text
physical pages
    ↓
GTT
    ↓
GPU virtual address
```

Do not assume legacy Gen8–11 GTT semantics are sufficient.

Determine whether the required Gen12 path needs:

* GGTT
* PPGTT
* per-context page tables
* context-specific GPU virtual address space

Use the minimum mechanism necessary for the copy engine.

## Requirements

* Preserve existing Gen8–11 GTT.
* Add Gen12-specific mapping only if required.
* Validate GPU addresses.
* Prevent overlapping mappings.
* Preserve CPU/GPU address separation.
* Keep ownership explicit.

No rendering yet.

---

# AL-5 — Gen12 Context / LRC Foundation

## Goal

Implement the minimum context state needed for Gen12 submission.

Gen12 must NOT reuse the legacy:

```text
RING_HEAD
RING_TAIL
RING_CTL
```

submission model blindly.

Investigate and implement the correct Gen12 context/submission mechanism supported by the target.

Potential mechanism:

```text
Logical Ring Context / LRC
```

or another documented modern submission mechanism.

Do not choose based on guesswork.

## Requirements

* Context allocation.
* Context memory layout.
* Required initialization.
* Required GPU virtual mappings.
* Context state validation.
* Lifecycle management.

No scheduler complexity beyond what is required for one serialized 2D submission owner.

---

# AL-6 — Gen12 Batch Buffer

## Goal

Create the minimum valid Gen12 batch buffer.

Required commands:

```text
COPY
FILL
BLIT
END
```

Use authoritative Gen12 command encoding.

Do not reuse Gen8–11 encoding unless explicitly verified to be compatible.

## Requirements

* Batch buffer allocation.
* Bounds checking.
* DWORD alignment.
* command-size validation.
* batch termination.
* GPU address validation.
* no buffer overflow.

Existing Phase-12 rectangle validation remains authoritative.

---

# AL-7 — Gen12 Completion / Fence

## Goal

Replace the legacy BCS completion mechanism with a Gen12-compatible completion mechanism.

Required semantics:

```text
submit
  ↓
GPU executes
  ↓
completion
  ↓
fence signaled
```

Existing public fence semantics should remain unchanged.

Internally the Gen12 implementation may use:

* memory completion values,
* status writes,
* interrupt completion,
* or another verified mechanism.

## Requirements

* monotonically increasing sequence.
* bounded wait.
* timeout handling.
* CPU fallback.
* SMP-safe state.
* no sleeping while holding submission locks.
* no unbounded polling.

---

# AL-8 — Minimal Gen12 Submission

## Goal

Submit exactly one minimal batch to the copy engine.

Do NOT integrate GHAL yet.

Pipeline:

```text
CPU
 ↓
Gen12 command buffer
 ↓
context
 ↓
copy engine
 ↓
completion
 ↓
fence
```

Start with the smallest possible operation.

For example:

```text
GPU writes known value
```

or the simplest verified copy/fill command.

## Success criteria

Actual Alder Lake hardware must execute the command.

Only then report:

```text
Gen12 submission: PASS
```

If not:

```text
BLOCKED
```

or:

```text
FAIL
```

depending on whether the failure is implementation vs hardware execution.

---

# AL-9 — Hardware COPY

Implement:

```text
src → dst
```

using actual Gen12 hardware.

Test:

* 64×64
* aligned
* unaligned
* subregion
* page boundary
* multiple pages

Validation must remain CPU-side.

Expected:

```text
CPU pattern
 ↓
Gen12 COPY
 ↓
fence
 ↓
CPU validation
```

Only report PASS after real GPU execution.

---

# AL-10 — Hardware FILL

Implement Gen12 fill.

Test:

1. full surface
2. small rectangle
3. edge rectangle
4. unaligned rectangle
5. multiple rectangles

Reuse the existing rectangle validation contract.

Do not duplicate validation logic unnecessarily.

---

# AL-11 — Hardware BLIT

Implement:

```text
source rectangle
      ↓
destination rectangle
```

Required contract remains:

```text
1:1
no scaling
```

Test:

* full copy
* subrectangle
* offset destination
* multiple BLITs
* cross-buffer

Same-buffer overlapping behavior must follow the existing contract.

Do not silently introduce undefined GPU overlap semantics.

---

# AL-12 — Gen12 Benchmark

Reuse the existing benchmark infrastructure.

Measure:

```text
CPU fill 64
CPU copy 64
CPU copy 256

Gen12 fill 64
Gen12 copy 64
Gen12 blit 64

damage 1 rect
damage 4 rect
damage 16 rect
damage 32 rect
```

Record:

* minimum
* average
* workload size
* submission overhead
* fence wait cost

Do not claim acceleration merely because the command executes.

Determine actual performance from measurements.

---

# AL-13 — GHAL Integration

Only after Gen12 hardware operations independently pass.

Architecture:

```text
GHAL
 │
Intel backend
 ├── Gen8–11 legacy BCS
 │
 └── Gen12 backend
        ↓
     COPY/FILL/BLIT
```

Backend selection:

```text
Intel Gen8–11 → legacy path
Intel Gen12+  → Gen12 path
unsupported    → software
```

Keep public GHAL API unchanged.

Do not expose Gen12-specific types through GHAL.

---

# AL-14 — Damage / Surface Integration

Verify existing GPU-backed surfaces work with Gen12.

Required:

```text
window surface
      ↓
damage region
      ↓
GHAL blit/fill
      ↓
Gen12 GPU
```

Do not redesign the compositor.

The compositor must remain unaware of:

* PCI
* Intel generation
* command buffers
* GPU registers
* contexts
* engines

---

# AL-15 — SMP Validation

Use:

```text
QEMU -smp 8
```

and physical Alder Lake where available.

Requirements:

* one submission owner
* serialized hardware submission
* no command-buffer corruption
* no fence sequence corruption
* no ring/context corruption
* no sleeping under spinlock
* CPU fallback remains safe

Stress:

```text
CPU0 → submit
CPU1 → submit
CPU2 → submit
...
CPU7 → submit
```

All submissions may be serialized internally.

Do NOT implement parallel GPU submission just for performance.

Only optimize after measuring contention.

---

# AL-16 — Robustness

Extend the existing robustness suite.

Invalid cases should include:

* invalid Gen12 context
* invalid GPU address
* unmapped GPU address
* null batch
* empty batch
* malformed command
* invalid rectangle
* invalid surface
* destroyed buffer
* invalid fence
* timeout
* submission failure
* context initialization failure

Expected behavior:

```text
reject
 ↓
bounded failure
 ↓
CPU fallback
```

Never:

```text
GPU error
 ↓
kernel panic
```

---

# AL-17 — Fault / Timeout Handling

Test bounded failure behavior.

Cases:

```text
submission timeout
fence timeout
invalid context
invalid engine
MMIO failure
allocation failure
```

The system must remain alive.

If Gen12 hardware becomes unusable:

```text
Gen12 acceleration disabled
software rendering continues
```

Do not implement a fake GPU reset.

Only implement reset if the actual hardware mechanism is understood, verified, and necessary.

---

# AL-18 — Final Gen12 Diagnostics

Final diagnostic should distinguish:

```text
GPU:
  backend: intel
  generation: Gen12
  engine: BCSx
  submission: gen12
  acceleration: enabled
```

or:

```text
GPU:
  backend: intel
  generation: Gen12
  submission: unavailable
  acceleration: disabled
  fallback: software
```

Never report:

```text
acceleration: enabled
```

merely because Intel PCI detection succeeded.

---

# AL-19 — Documentation

Update/create:

```text
GPU_ARCHITECTURE.md
GPU_DRIVER.md
GPU_MEMORY.md
GPU_COMMANDS.md
GPU_ACCELERATION_BENCHMARK.md
```

Document:

* actual hardware
* device ID
* revision
* BAR
* engine discovery
* Gen12 submission model
* context model
* GPU VM model
* command encoding
* fence mechanism
* synchronization
* SMP behavior
* fallback
* benchmark results
* known limitations

Separate:

```text
implemented
verified
SKIP
BLOCKED
```

Do not hide limitations.

---

# AL-20 — Final Validation

Run:

```text
clean build
QEMU -smp 1
QEMU -smp 4
QEMU -smp 8
```

and physical Alder Lake validation.

Verify:

```text
build: PASS
boot: PASS
robustness: PASS
software fallback: PASS
Gen12 discovery: PASS
Gen12 submission: PASS
COPY: PASS
FILL: PASS
BLIT: PASS
fence: PASS
SMP: PASS
benchmark: PASS
```

Only mark hardware items PASS when they actually executed on physical Gen12 hardware.

---

# Important Engineering Constraints

## Keep legacy path intact

Do not replace:

```text
Gen8–11 legacy BCS
```

with Gen12 code.

Use dispatch:

```text
generation == Gen12
    → gen12 backend

generation == Gen8–11
    → legacy backend

otherwise
    → software
```

---

## Submission ownership

Maintain:

```text
one submission owner
```

Even if multiple CPUs create commands.

Initial design:

```text
CPU workers
    ↓
g_bcs_lock / gen12_submit_lock
    ↓
single hardware submission
```

Only optimize this after measuring real contention.

---

## Lock hierarchy

Do not create lock cycles.

Maintain the existing discipline:

```text
buffer
 ↓
GTT / VM
 ↓
fence
 ↓
submission
```

Never sleep/HLT while holding a spinlock.

---

## Failure model

Every hardware failure must converge toward:

```text
Gen12 unavailable
      ↓
Intel acceleration disabled
      ↓
GHAL fallback
      ↓
software backend
```

Graphics failure must never crash KyuzenOS.

---

# Definition of Done

The Alder Lake extension is complete only when:

1. Actual Intel GPU is identified from PCI.
2. Gen12 is correctly identified.
3. MMIO access is validated.
4. Required copy engine is identified.
5. Required GPU VM mechanism works.
6. Required context/submission mechanism works.
7. A real command executes on Alder Lake.
8. Fence completion works.
9. COPY works.
10. FILL works.
11. BLIT works.
12. GHAL uses Gen12 automatically.
13. Existing compositor/damage system remains unchanged.
14. SMP stress passes.
15. Robustness passes.
16. Software fallback remains functional.
17. Benchmarks produce real hardware numbers.
18. Documentation reflects reality.
19. No Gen12-specific details leak into the frozen GHAL API.
20. No unsupported claims are marked PASS.

# Critical Instruction to Coding Agent

Work **one phase at a time**.

For each phase:

1. inspect the existing implementation,
2. inspect authoritative hardware information,
3. implement the smallest required change,
4. build,
5. run available tests,
6. report PASS/SKIP/BLOCKED/FAIL honestly,
7. document important discoveries,
8. do not continue to the next phase until the current phase is understood.

If hardware information is missing, **stop and gather it instead of guessing**.

Do not perform large speculative refactors.

Do not rewrite working Phase 0–25 code.

The objective is not to create a large Intel GPU driver.

The objective is:

> **Make KyuzenOS's existing 2D GPU architecture execute real COPY/FILL/BLIT workloads on the user's Alder Lake iGPU while preserving the frozen architecture and software fallback.**
