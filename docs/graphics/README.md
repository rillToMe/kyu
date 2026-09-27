# Graphics

KyuzenOS provides 2D hardware-accelerated graphics through a **Graphics HAL
(GHAL)**, a dirty-region **compositor**, and a **window manager (KWM)**. Three
backends are supported: a software backend, VirtIO-GPU, and the Intel
integrated GPU.

## Overview

| Layer | Location | Responsibility |
| --- | --- | --- |
| Graphics HAL | `graphics/ghal.c`, `graphics/ghal.h` | Backend registry, surface API, dispatch |
| Backends | `graphics/backend/` | Software, VirtIO-GPU, Intel |
| Compositor | `kernel/gfx/compositor.c` | Dirty-region tracking, base blit, present |
| Window manager | `kernel/gfx/kwm.c` | Windows, z-order, focus, decoration, drag |
| Display / framebuffer | `kernel/display.c`, `kernel/gfx/fb.c` | `DisplayBuffer`, `Viewport`, primitives |

| Document | Description |
| --- | --- |
| [Compositor](compositor.md) | Dirty regions, occlusion, base-blit elimination, present |
| [Window Manager](window-manager.md) | Windows, ownership, z-order, input routing |
| [Backends](backends.md) | GHAL, software, VirtIO-GPU, Intel |
| [Display & Primitives](display.md) | DisplayBuffer, Viewport, primitive API, color format |

## Graphics HAL (GHAL)

The GHAL is the single abstraction between the compositor and the hardware. It
maintains a registry of backends and an active backend, and dispatches surface
operations.

### Capability flags

| Flag | Value | Meaning |
| --- | --- | --- |
| `GHAL_CAP_HW_CURSOR` | `1 << 0` | Hardware cursor plane |
| `GHAL_CAP_ASYNC_PRESENT` | `1 << 1` | Present returns before completion (fence) |
| `GHAL_CAP_PARTIAL_FLUSH` | `1 << 2` | Partial-region flush |
| `GHAL_CAP_MODE_SET` | `1 << 3` | Runtime mode-setting |

### Backend selection

`ghal_init()` tries backends in a fixed order; the first whose `init()`
succeeds becomes active:

1. **VirtIO-GPU** (`graphics/backend/virtio_gpu.c`)
2. **Intel** (`graphics/backend/intel/intel_init.c`)
3. **Software** (`graphics/backend/software.c`) — never fails

If no backend initializes, the HAL has no active backend and returns `-1`.
Initialization is idempotent.

### Surfaces

A `ghal_surface_t` is **opaque** — each backend defines its own concrete
struct. Formats:

| Format | Value | Use |
| --- | --- | --- |
| `GHAL_FMT_XRGB8888` | 0 | Native framebuffer |
| `GHAL_FMT_ARGB8888` | 1 | Cursor only |

`GHAL_MAX_DIM` = 8192 bounds all surface dimensions.

### API-freeze rule

The backend vtable is **append-only**: new operations are added at the end and
NULL-checked in dispatch; fields are never reordered, and backend-specific
types (for example, Intel types) never leak into the HAL header. This keeps
the `gpu_stats_t` mirror in `include/userlib.h` (syscall 65) in sync, enforced
by a `_Static_assert`.

## Compositor at a Glance

The compositor builds each frame from a background layer (`base_canvas`) plus
windows, and presents only the regions that changed. It uses three buffers and
a scanout surface:

| Buffer | Role |
| --- | --- |
| `base_canvas` | Persistent background (TTY, HUD, wallpaper) |
| `backbuffer` | Rebuilt each frame from base + windows |
| `fb_ptr` | Hardware framebuffer |
| `g_main_surface` | GHAL scanout surface |

Key techniques:

- **Dirty regions** — only changed rectangles are recomposited and presented.
- **Base-blit elimination** — opaque windows let the compositor skip copying
  the background underneath them.
- **Occlusion culling** — fully covered window content is not drawn.
- **Opaque fast path** — opaque windows are `memcpy`-blitted.
- **Present union** — nearby dirty rectangles are merged into one present.

See [Compositor](compositor.md) for the algorithms and constants.

## Color Contract

The native format is **XRGB8888**: the low 24 bits are `0xRRGGBB`, and the top
byte is an opacity mask (0 = transparent, non-zero = opaque). This matches every
layer (base canvas, backbuffer, window canvas, compositor) with no mid-pipeline
conversion.

## Related Documentation

- [Compositor](compositor.md)
- [Window Manager](window-manager.md)
- [Backends](backends.md)
- [Display & Primitives](display.md)
- [GUI](../gui/README.md) — the widget toolkit and desktop built on top
