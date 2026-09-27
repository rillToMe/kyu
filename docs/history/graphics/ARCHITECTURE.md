# KyuzenOS — Hardware Accelerated 2D Graphics

## 1. Overview

The graphics subsystem decouples the GUI from the rendering hardware. All
higher layers (GUI toolkit, window manager, compositor) call a single,
vendor-agnostic **Graphics HAL**. Rendering is performed by a pluggable
**backend**; today the **software backend** implements the HAL on the CPU, and
future accelerated backends (VirtIO GPU, VMware SVGA II, Bochs, Intel, AMD,
NVIDIA) will implement the *same* interface — applications never know which is
active.

This is **not** a 3D engine. It accelerates 2D framebuffer composition while
remaining fully compatible with the existing software renderer.

## 2. Architecture

```
 Application
     │
     ▼
 GUI Toolkit
     │
     ▼
 Window Manager
     │
     ▼
 Compositor
     │
     ▼
 2D Graphics API        (graphics/renderer/graphics.c — gfx_*)
     │
     ▼
 Graphics HAL           (graphics/hal/gpu.c — gpu_* dispatch)
     │
     ▼
 GPU Driver (backend)   (graphics/backend/software/software_gpu.c — gpu_backend_ops)
     │
     ▼
 Framebuffer / Hardware
```

Each layer depends only on the one directly below it. Backend choice is an
implementation detail of the HAL.

## 3. Source tree

```
graphics/
├── hal/
│   ├── gpu.c            # backend registry + HAL dispatch (gpu_register_backend, gpu_*)
│   └── surface.c        # surface helpers: clipping, row access
├── backend/
│   └── software/
│       ├── software_gpu.c   # software backend: fill/line/blit/stretch/blend/present
│       └── software_gpu.h   # software_gpu_register()
├── surface/
│   └── surface_manager.c    # surface pool, handles, refcount, VRAM heap
├── renderer/
│   └── graphics.c           # high-level 2D API (gfx_*), damage accumulation
├── include/graphics/        # public headers (mirrored to driver/graphics/)
│   ├── gpu.h
│   ├── surface.h
│   ├── surface_manager.h
│   └── graphics.h
└── CMakeLists.txt           # optional standalone build
```

Public headers also live under `include/graphics/` (kernel include root) and are
mirrored to `driver/graphics/` for driver-port consumers.

## 4. Graphics HAL (`gpu.h`)

The HAL is a vtable of backend operations (`gpu_backend_ops_t`):

| Op                 | Purpose                                      |
|--------------------|----------------------------------------------|
| `initialize`       | bring a backend up                            |
| `shutdown`         | tear a backend down                          |
| `create_surface`   | validate/register a surface with a backend   |
| `destroy_surface`  | release backend-side surface state           |
| `upload_texture`   | copy host bytes into a surface               |
| `fill_rect`        | solid rect fill                              |
| `draw_line`        | Bresenham line                               |
| `draw_image`       | opaque bitmap copy                           |
| `blit`             | blit with optional alpha / color-key         |
| `stretch_blit`     | scaled blit                                  |
| `alpha_blend`      | source-over alpha blending                   |
| `present`          | submit damaged regions to the front buffer   |
| `flush`            | drain the command pipeline                   |
| `wait_idle`        | block until all pending work completes       |

`gpu_register_backend()` registers a `gpu_backend_t`; `gpu_initialize()` probes
each registered backend and activates the first that succeeds. `gpu_*` wrappers
route to the active backend and are NULL-safe (return `GPU_ERR_NO_GPU` when none
is active). No vendor registers ever appear above a backend.

## 5. Surface system (`surface.h`, `surface_manager.c`)

A `gpu_surface_t` stores:

- geometry: `width`, `height`, `stride`
- `format` (XRGB8888 / ARGB8888 / RGB565)
- `memory` (SYSTEM / VRAM / NONE)
- `kind` (front / back / window / offscreen / texture)
- `handle`, `refcount`, `vram_offset`
- `pixels` (CPU-addressable) + ownership flag

The surface manager owns a **bounded static pool** (`GPU_MAX_SURFACES`), assigns
unique handles, and reference-counts surfaces (`retain` / `release`). System-memory
pixel backing is allocated with `kmalloc` and freed on final release. All
pool/refcount/vram mutation is serialized by one spinlock (SMP-safe).

## 6. GPU memory manager

`gpu_vram_alloc(size, align)` / `gpu_vram_free(offset, size)` implement a simple
first-fit bitmap allocator over a simulated 64 MiB VRAM region. Dedicated-VRAM
backends use this to allocate device memory; shared-memory backends use the
system `pixels` field directly. Alignment defaults to 4096 bytes.

## 7. Double buffering & dirty rectangles

The renderer (`graphics.c`) owns a back buffer and accumulates a **damage
rectangle** as primitives are drawn (`gfx_fill_rect`, `gfx_blit`, ...). On
`gfx_present(canvas, full)`, only damaged regions are submitted to
`gpu_present()`, so the software backend / a partial-swap hardware path avoids
copying the whole framebuffer.

## 8. Software backend

`software_gpu.c` is the reference implementation:

- pixel format: XRGB8888 (top byte = opaque mask / alpha)
- `fill_rect`: clipped row fill
- `draw_line`: integer Bresenham
- `blit`/`stretch_blit`: clipped, source/dest scale-aware; `GPU_BLEND_ALPHA`
  source-over blending, `GPU_BLEND_KEY` skips top-byte-0 pixels
- `present`: no-op today (front buffer wiring is the compositor's job)

Every accelerated backend must produce the same visual output for the same
inputs.

## 9. Concurrency

- Registry mutation is guarded by `g_gpu_registry_lock`.
- Surface manager pool/refcount/vram guarded by its own spinlock.
- Pixel backing is allocated **outside** the manager lock (`kmalloc` may sleep).
- The software backend keeps a per-instance lock so two CPUs drawing to the same
  surface cannot interleave.

## 10. Adding a new backend (future guide)

1. Create `graphics/backend/<name>/<name>_gpu.c`.
2. Implement every member of `gpu_backend_ops_t` (see the software backend as a
   template; unsupported ops may return `GPU_ERR_UNSUPPORTED`).
3. Expose a `static gpu_backend_t` and a `<name>_gpu_register()` that calls
   `gpu_register_backend()`.
4. Add the dir to `SRC_DIRS` in the Makefile (and the CMake source list).
5. Call `<name>_gpu_register()` before `gpu_initialize()` (e.g. from
   `gfx_graphics_init()`). `gpu_initialize()` activates the first backend that
   succeeds, so priority = registration order.

Recommended order: software → virtio-gpu → svga → bochs → intel → amd → nvidia.

## 11. Integration points

The existing `kernel/gfx/fb.c` and `kernel/gfx/compositor.c` remain the active
render path. The new HAL is additive: the compositor can be migrated incrementally
to call `gfx_*`/`gpu_*` for its primitives without replacing the framebuffer.
`gfx_graphics_init()` (renderer) is the entry point that registers the software
backend, initializes the HAL, and creates front/back surfaces.

## 12. Dokumen terkait

| Dokumen | Isi |
|---------|-----|
| [`API.md`](API.md) | Referensi lengkap API HAL / surface / surface manager / renderer + contoh penggunaan |
| [`BACKEND_GUIDE.md`](BACKEND_GUIDE.md) | Panduan menambah backend GPU baru (VirtIO, SVGA, Bochs, Intel, AMD, NVIDIA) |

## 13. Phase 1.5 — Implementation status & decisions

Phase 1.5 completes the software foundation (no hardware acceleration).

### Implemented & validated

- **Software backend present**: `present()` now copies damaged regions from the
  back surface (`backend->back`, set by the renderer) to the front surface.
- **Renderer double buffering**: `gfx_graphics_init_with()` wraps a caller-provided
  front buffer (e.g. the hardware framebuffer) and allocates a private back buffer;
  `gfx_present()` submits only the damaged rect.
- **Compositor present bridge**: `gfx_compositor_present(back, front, w, h, damage)`
  routes a raw back→front damage copy through the HAL (additive; compositor may
  keep its direct path).
- **VRAM stats**: `gpu_vram_stats()` reports total/used/peak/allocations. VRAM
  allocations never return offset 0 (reserved) so 0 stays a valid "none" sentinel.
- **Backend self-test**: `software_gpu_selftest()` (runs under `GFX_SELFTEST`)
  validates surface create/refcount, fill+clip, blit (opaque + color-key),
  alpha blend, stretch blit, VRAM alloc/free, and NULL-safety. Verified **PASS**
  in QEMU.

### Implementation decisions

- `fill_rect` writes `color & 0xFFFFFF` (XRGB: top byte is the opaque/alpha mask,
  so a solid fill is always opaque). To create a source pixel with real alpha,
  write the surface memory directly (see the self-test).
- `blit`/`stretch_blit` store `sp & 0xFFFFFF` (RGB only); the alpha byte is used
  for blending/keying, not stored in the target surface.
- Concurrency: registry, surface pool, and VRAM heap are spinlock-guarded.
  Concurrent pixel writes to the SAME surface are the caller's responsibility
  (matching the existing compositor model). No per-op locking on hot paths.

### Remaining for Phase 2

- First real hardware backend (VirtIO GPU, then SVGA/Bochs/Intel/AMD/NVIDIA).
- GPU command queue & batch rendering.
- Hardware present / page flip / partial swap.
- Hardware resource allocation + texture upload/download.
- Hardware synchronization (`wait_idle` with real device idle).
- Migrate the live compositor's draw path (`kernel/gfx/compositor.c`) to the
  `gfx_*`/`gpu_*` API (currently the present bridge is available but the compositor
  keeps its direct `blit_rect_db` path to guarantee identical output).


