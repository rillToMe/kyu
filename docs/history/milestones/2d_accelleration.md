# KyuzenOS — 2D Hardware GPU Acceleration

> **Prioritas**: MAX
> **Status**: **DONE** (2026-09-20) 
## Mission

Implement a clean, modular, hardware-accelerated 2D graphics subsystem for KyuzenOS.

The immediate target is the Intel integrated GPU available on the development machine, using its hardware 2D/blitter capabilities where supported.

This project is strictly a **2D acceleration project**.

Do NOT implement or introduce:

* 3D rendering
* OpenGL
* Vulkan
* shader pipelines
* compute shaders
* Mesa
* a general-purpose 3D graphics API
* a full desktop GPU stack
* unnecessary display-server abstractions

The goal is to accelerate operations currently performed by the CPU software compositor:

* buffer copy
* rectangle fill
* blit
* surface movement
* basic 2D compositing where hardware support is appropriate

The existing software compositor must remain functional as a fallback.

---

# Architectural Principles

These rules are authoritative for the entire project.

## 1. Hardware abstraction first

Kyuzen graphics code must NOT depend directly on Intel hardware.

Use:

```
KWM / compositor
        |
        v
    GPU HAL
        |
  +-----+------+
  |            |
```

software      Intel
backend       backend

The compositor must only interact with the GPU HAL.

Intel-specific code must stay inside the Intel backend.

---

## 2. Software fallback is mandatory

Hardware acceleration is an optimization, not a requirement for booting or basic graphics.

If:

* GPU detection fails
* GPU initialization fails
* unsupported hardware is detected
* command submission fails
* a GPU operation is unsupported

the system must be able to fall back to software rendering.

The fallback must not crash the kernel.

---

## 3. No assumptions about the exact Intel GPU

The CPU is an Intel Core i5-12450HX.

However, do NOT hardcode an assumed PCI device ID.

The actual GPU must be detected through PCI enumeration.

Device-specific implementation must only be enabled after identifying:

* PCI vendor ID
* PCI device ID
* PCI class
* PCI subclass
* PCI BAR layout
* exact supported GPU generation/platform

Do not write register programming based on assumptions.

---

## 4. No premature optimization

First make the hardware path correct.

Then benchmark.

Then optimize based on measurements.

Do not add:

* complex batching
* speculative caching
* complicated schedulers
* lock-free structures
* multiple command queues
* asynchronous execution

until profiling demonstrates a need.

---

## 5. Keep GPU code independent from KWM

GPU code must not know about:

* windows
* KWM
* widgets
* libui
* damage regions

The GPU layer only understands:

* buffers
* surfaces
* rectangles
* commands
* fences
* synchronization

KWM remains responsible for deciding WHAT needs to be rendered.

GPU HAL is responsible for HOW the operation is executed.

---

# Target Architecture

The intended architecture is:

```
Application
    |
   libui
    |
   KWM
    |
compositor
    |
damage regions
    |
GPU HAL
  /     \
```

software    Intel
backend     backend
|            |
CPU          Intel GPU
2D engine
|
framebuffer

---

# Phase 0 — Architecture Freeze

Before implementation:

1. Inspect the existing graphics architecture.
2. Identify:

   * framebuffer abstraction
   * display abstraction
   * GHAL
   * KWM
   * compositor
   * damage system
   * existing memory allocators
   * PCI subsystem
   * interrupt subsystem
   * physical memory manager
   * virtual memory manager
3. Document integration points.
4. Do NOT modify KWM yet.
5. Do NOT modify the compositor yet.
6. Do NOT implement GPU commands yet.

Deliverable:

```
GPU_ARCHITECTURE.md
```

containing:

* subsystem boundaries
* ownership rules
* buffer lifetime rules
* synchronization model
* fallback behavior
* planned driver interface

This phase establishes the architecture and prevents later redesign.

---

# Phase 1 — GPU HAL

Create the generic GPU abstraction.

Suggested location:

```
kernel/gpu/
```

Possible structure:

```
kernel/gpu/
    gpu.c
    gpu.h
    gpu_device.c
    gpu_buffer.c
    gpu_fence.c
    gpu_queue.c
```

The exact structure may differ if existing Kyuzen conventions require it.

Define generic concepts:

```
gpu_device
gpu_buffer
gpu_surface
gpu_rect
gpu_fence
gpu_command
gpu_backend
```

The API should support at minimum:

```
gpu_init()
gpu_shutdown()

gpu_buffer_create()
gpu_buffer_destroy()

gpu_buffer_map()
gpu_buffer_unmap()

gpu_copy()
gpu_fill()
gpu_blit()

gpu_submit()
gpu_wait()
```

Do not expose Intel registers or Intel-specific types in this API.

Deliverable:

A generic GPU HAL that can theoretically support multiple backends.

---

# Phase 2 — Software GPU Backend

Implement the first backend using the existing CPU renderer.

This is intentionally done BEFORE the Intel backend.

Operations:

```
software_copy()
software_fill()
software_blit()
```

The software backend must implement the exact semantics expected by the GPU HAL.

This establishes:

* API correctness
* buffer semantics
* rectangle semantics
* clipping behavior
* synchronization semantics

Tests must prove that the generic GPU HAL behaves correctly even without hardware acceleration.

Deliverable:

```
GPU HAL + software backend
```

with all tests passing.

---

# Phase 3 — PCI GPU Detection

Integrate with the existing PCI subsystem.

Find display controllers:

```
class    = 0x03
subclass = 0x00
```

Identify:

```
vendor ID
device ID
revision
BARs
interrupt information
```

For Intel:

```
vendor ID = 0x8086
```

Do NOT assume the GPU model.

Produce diagnostic output similar to:

```
GPU:
  vendor: 8086
  device: XXXX
  revision: XX
  class: 03
  subclass: 00
  BAR0: XXXXXXXX
  BAR2: XXXXXXXX
```

Determine the exact GPU generation from the detected device ID.

Only after this phase may device-specific register programming begin.

Deliverable:

Reliable GPU identification.

---

# Phase 4 — Intel Driver Skeleton

Create the Intel backend.

Suggested structure:

```
kernel/gpu/intel/
    intel.c
    intel.h
    intel_pci.c
    intel_mmio.c
    intel_regs.h
    intel_memory.c
    intel_gtt.c
    intel_bcs.c
    intel_fence.c
    intel_irq.c
```

The exact filenames may change according to the existing project style.

Responsibilities:

```
PCI integration
BAR mapping
MMIO access
GPU generation detection
feature detection
```

At this stage:

```
detect GPU
map required MMIO
verify MMIO access
```

Do not submit commands yet.

Deliverable:

```
Intel GPU detected
Intel backend initialized
MMIO access verified
```

---

# Phase 5 — Intel GPU Memory Management

Implement the minimum memory abstraction needed for 2D operations.

The iGPU uses system memory, so design the memory system around:

```
physical memory
      |
    pages
      |
   GPU-visible memory
      |
     GTT
      |
GPU virtual address
```

Do NOT immediately build a generalized VRAM manager.

Implement only what is required for 2D acceleration.

Required concepts:

```
gpu_buffer
physical backing pages
GPU virtual address
mapping/unmapping
CPU mapping
```

The system must correctly track:

* physical pages
* GPU virtual addresses
* buffer size
* CPU accessibility
* GPU accessibility
* ownership/lifetime

Deliverable:

A GPU buffer can be created and mapped for GPU use.

---

# Phase 6 — GTT / GPU Virtual Memory

Implement the minimum GPU address-space support required by the detected Intel generation.

Goals:

```
CPU physical memory
      |
      v
    GTT
      |
      v
GPU virtual address
```

Implement:

* GPU address allocation
* page mapping
* page unmapping
* buffer mapping
* buffer destruction

Do not build a generalized GPU VM system beyond what 2D acceleration needs.

Deliverable:

A GPU buffer has a valid GPU-visible address.

---

# Phase 7 — Intel Blitter / BCS Initialization

Target the Intel 2D blitter engine.

Do NOT initialize:

* 3D render engine
* shader pipeline
* compute engine
* Vulkan
* OpenGL

Only initialize the hardware path necessary for:

```
copy
blit
fill
```

Determine the exact command format and register layout from documentation corresponding to the detected GPU generation.

Do NOT copy register values from another GPU generation without verification.

Deliverable:

The Intel blitter engine is initialized and reported as available.

---

# Phase 8 — GPU Command Buffer

Implement the minimum command submission infrastructure.

Concepts:

```
command buffer
command allocation
command writing
command submission
completion tracking
```

The system must be able to generate a hardware command sequence for a basic 2D operation.

Do not implement a general-purpose GPU scheduler.

Start with one queue / one engine.

Deliverable:

Kyuzen can submit a valid command to the Intel 2D engine.

---

# Phase 9 — First Hardware Test: COPY

Implement the smallest possible hardware test.

Create:

```
source buffer
destination buffer
```

Initialize source with known data.

Submit a GPU copy.

Wait for completion.

Validate destination contents using the CPU.

Example:

```
source:
    AAAA
    BBBB
    CCCC
    DDDD

GPU COPY

destination:
    ....
    BBBB
    CCCC
    ....
```

Expected result:

```
GPU COPY: PASS
```

This is the first major hardware milestone.

Do not connect this to KWM yet.

Deliverable:

```
Intel GPU hardware COPY works correctly.
```

---

# Phase 10 — Hardware FILL

Implement rectangle fill.

Required operation:

```
gpu_fill(
    buffer,
    rectangle,
    color
)
```

Test:

```
clear buffer
fill rectangle
CPU validate result
```

Test:

* full-buffer fill
* small rectangle
* edge rectangle
* clipped rectangle
* unaligned rectangle if hardware permits
* multiple rectangles

Deliverable:

```
GPU FILL: PASS
```

---

# Phase 11 — Hardware BLIT

Implement surface-to-surface 2D blitting.

Required:

```
gpu_blit(
    source,
    destination,
    source_rect,
    destination_rect
)
```

Initially support only the simplest guaranteed operation:

```
1:1 copy
```

Then, only if the hardware path naturally supports it:

```
scaling
```

Do not introduce scaling if it complicates the driver unnecessarily.

Deliverable:

```
GPU BLIT: PASS
```

---

# Phase 12 — Rectangle / Boundary Validation

Build comprehensive tests for:

* zero-size rectangles
* negative coordinates
* out-of-bounds rectangles
* clipping
* source bounds
* destination bounds
* overlapping source/destination
* buffer size mismatch
* invalid buffers

The GPU backend must never trust userspace input.

Kernel validation is mandatory.

Deliverable:

No invalid GPU operation can corrupt arbitrary memory.

---

# Phase 13 — Fence / Completion

Implement GPU completion tracking.

Required abstraction:

```
gpu_fence_t
```

Flow:

```
command generation
      |
   submit
      |
   fence
      |
GPU execution
      |
  completion
      |
   fence signaled
```

Minimum API:

```
gpu_submit()
gpu_fence_wait()
gpu_fence_is_signaled()
```

Start synchronously.

Asynchronous operation can be added later.

Deliverable:

CPU can reliably determine when GPU work has completed.

---

# Phase 14 — Interrupt-Based Completion

After polling/synchronous completion is proven, integrate GPU completion interrupts if appropriate for the detected hardware.

Flow:

```
GPU completes command
       |
    interrupt
       |
  Intel IRQ handler
       |
  signal fence
       |
   waiting task
```

Do not introduce complicated interrupt threading unless required by the existing kernel architecture.

Reuse existing Kyuzen interrupt infrastructure.

Deliverable:

GPU completion can wake waiting tasks without busy waiting.

---

# Phase 15 — GPU HAL Integration

Connect the Intel backend to the generic GPU HAL.

Final backend selection:

```
gpu_init()
    |
    +-- Intel detected
    |      |
    |      +-- Intel backend
    |
    +-- unsupported
           |
           +-- software backend
```

The rest of Kyuzen must not know which backend is active.

Diagnostic:

```
GPU:
  backend: Intel
  acceleration: enabled
  engine: BCS
```

or:

```
GPU:
  backend: software
  acceleration: disabled
```

Deliverable:

Transparent backend selection.

---

# Phase 16 — GPU Surface Abstraction

Introduce the concept of a graphics surface.

A surface represents:

```
width
height
stride
pixel format
buffer
GPU address
CPU mapping state
```

Initially support the existing Kyuzen framebuffer pixel format.

Do not introduce multiple pixel formats unless necessary.

Deliverable:

KWM-compatible surfaces can exist independently from the physical display framebuffer.

---

# Phase 17 — GPU-Backed Window Surfaces

Modify KWM integration carefully.

Windows should continue to own logical surfaces.

Instead of immediately writing every pixel directly to the final framebuffer:

```
Window
  |
  v
Surface
  |
  v
compositor
```

The compositor decides which operations can be sent to the GPU.

Do not redesign the window manager.

Do not change window lifetime semantics.

Deliverable:

KWM can provide surfaces suitable for GPU operations.

---

# Phase 18 — Hardware-Accelerated Damage Composition

Connect the existing damage system to GPU operations.

Existing Kyuzen concepts such as:

* damage regions
* dirty rectangles
* opaque windows
* opaque splitting
* localized window destruction
* partial redraw

must remain intact.

Pipeline:

```
damage regions
      |
      v
compositor planning
      |
      v
GPU operations
      |
      v
framebuffer
```

Only damaged regions should be submitted.

Do NOT switch to full-screen GPU rendering.

Deliverable:

KWM can use GPU acceleration while preserving the existing damage architecture.

---

# Phase 19 — GPU / CPU Synchronization

Implement synchronization between:

```
CPU writes
GPU reads
GPU writes
CPU reads
```

Avoid unnecessary global synchronization.

The initial implementation may use explicit waits.

Then optimize only after profiling.

Required correctness:

```
CPU -> GPU
GPU -> CPU
GPU -> GPU
```

must all be deterministic.

Deliverable:

No tearing or stale-buffer corruption caused by missing synchronization.

---

# Phase 20 — Benchmark

Benchmark the existing software compositor against GPU acceleration.

Measure separately:

```
damage calculation
command generation
CPU copy
CPU blend
GPU submission
GPU execution
synchronization
framebuffer presentation
```

Test workloads:

```
1 window
4 windows
16 windows
32 windows
large opaque windows
transparent windows
small damage
large damage
full-screen damage
window movement
window destruction
scrolling
```

Do not assume GPU acceleration is faster.

Record actual measurements.

Deliverable:

```
GPU_ACCELERATION_BENCHMARK.md
```

---

# Phase 21 — Optimize Only Based on Measurements

Potential optimizations, ONLY if benchmarks justify them:

* command batching
* persistent GPU buffers
* buffer reuse
* fence reuse
* asynchronous submission
* multiple GPU commands per submission
* damage-region batching
* CPU/GPU overlap
* per-CPU command construction
* surface caching

Do not implement these speculatively.

Deliverable:

Measured performance improvement with no regression in correctness.

---

# Phase 22 — Robustness / Fallback

Test failure scenarios:

* unsupported Intel GPU
* GPU initialization failure
* MMIO failure
* invalid buffer
* invalid command
* GPU timeout
* fence timeout
* command submission failure
* GPU reset if applicable

The system must fall back to software rendering when practical.

A graphics failure must not unnecessarily bring down the entire OS.

Deliverable:

Graceful hardware acceleration failure.

---

# Phase 23 — SMP Integration

Only after the single-CPU GPU path is stable.

Integrate with Kyuzen's existing SMP architecture.

Initially:

```
one GPU submission owner
```

Multiple CPUs may generate work, but GPU submission synchronization must be explicit.

Avoid creating a complicated GPU scheduler.

Later, if profiling demonstrates contention:

```
per-CPU command buffers
submission batching
lock reduction
```

may be introduced.

Deliverable:

GPU acceleration remains correct under SMP.

---

# Phase 24 — API Stabilization

Freeze the public GPU HAL.

The final abstraction should expose concepts similar to:

```
gpu_device
gpu_buffer
gpu_surface
gpu_rect
gpu_fence
```

and operations:

```
create_buffer
destroy_buffer
map
unmap
copy
fill
blit
submit
wait
```

Intel-specific details must remain private to the Intel backend.

Deliverable:

Stable internal GPU API.

---

# Phase 25 — Documentation

Document:

```
GPU_ARCHITECTURE.md
GPU_DRIVER.md
GPU_MEMORY.md
GPU_COMMANDS.md
GPU_ACCELERATION_BENCHMARK.md
```

Document:

* hardware assumptions
* detected GPU
* supported operations
* buffer lifetime
* synchronization
* fallback behavior
* command submission
* known limitations
* benchmark results

---

# Explicit Non-Goals

The following are OUT OF SCOPE for this project:

* 3D acceleration
* Vulkan
* OpenGL
* OpenGL ES
* shader compilation
* compute shaders
* Mesa
* full DRM/KMS implementation
* GPU desktop driver compatibility
* CUDA
* video decode
* video encode
* ray tracing
* general-purpose GPU compute
* multi-GPU support
* discrete NVIDIA acceleration
* discrete AMD acceleration
* advanced color management
* HDR
* variable refresh rate
* advanced display pipelines

These may be future projects, but they must not expand the current scope.

---

# Final Target

The completed system should look like:

```
KyuzenOS
    |
    +-- KWM
    |
    +-- compositor
    |
    +-- damage system
    |
    +-- GPU HAL
          |
          +-- software backend
          |
          +-- Intel backend
                 |
                 +-- PCI
                 +-- MMIO
                 +-- GPU memory
                 +-- GTT
                 +-- BCS
                 +-- command buffers
                 +-- fences
                 +-- interrupts
```

Final rendering flow:

```
Window surfaces
      |
      v
damage calculation
      |
      v
compositor planning
      |
      v
GPU HAL
      |
      v
Intel 2D engine
      |
      v
display framebuffer
```

The CPU should primarily perform:

```
scheduling
damage calculation
command generation
synchronization
```

The GPU should perform:

```
copy
fill
blit
supported 2D operations
```

The software renderer remains available as a fallback at all times.

---

# Agent Rules

1. Do not skip phases.
2. Do not combine unrelated phases.
3. Do not redesign previous architecture without concrete evidence.
4. Do not change the GPU HAL because of an Intel-specific implementation detail.
5. Do not add 3D functionality.
6. Do not assume the Intel device ID.
7. Do not write undocumented register values.
8. Verify the exact GPU generation before implementing hardware commands.
9. Reuse existing Kyuzen PCI, PMM, VMM, IRQ, SMP, framebuffer, and graphics infrastructure.
10. Do not duplicate existing subsystems.
11. Prefer the smallest implementation that satisfies the current phase.
12. Every phase must have a concrete validation/test.
13. Preserve the software renderer throughout the project.
14. Benchmark before claiming an optimization is beneficial.
15. Do not modify KWM until the hardware path has passed standalone tests.
16. Do not introduce a dependency on Linux kernel APIs.
17. Linux i915/Nouveau/Mesa source may be used as technical reference, but Kyuzen must implement its own OS-specific interfaces.
18. Keep Intel-specific code isolated.
19. Keep hardware register definitions documented and generation-specific.
20. Stop and report blockers instead of inventing hardware behavior.

# Completion Criterion

The project is complete when:

1. Kyuzen detects the actual Intel iGPU.
2. Intel MMIO is initialized safely.
3. Required GPU memory/GTT functionality works.
4. The Intel 2D blitter is initialized.
5. GPU COPY works.
6. GPU FILL works.
7. GPU BLIT works.
8. Fences work.
9. Interrupt-based completion works where supported.
10. GPU HAL exposes the Intel backend.
11. Software fallback remains functional.
12. KWM can submit damaged 2D operations through the GPU HAL.
13. Existing damage/opaque-region optimizations remain correct.
14. SMP execution is safe.
15. Benchmarks demonstrate the actual performance characteristics.
16. No 3D subsystem has been introduced.

The primary objective is not "make a GPU driver".

The primary objective is:

```
**Move Kyuzen's existing 2D compositor workload from CPU software execution to the Intel GPU's dedicated 2D hardware path, while keeping the graphics architecture modular and software-fallback capable.**
```
