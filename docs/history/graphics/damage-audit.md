# Damage pipeline audit

## Before changes (2026-09-24)

1. **Origin:** `libs/core/libgui.c` primitives accumulate a clipped window-local
   bounding box; `gui_flush` submits syscall 31/66. Rust's `upload_region`
   submits a bounded list of local rectangles. Desktop wallpaper reload requests
   a full desktop-canvas render; taskbar/preview updates use canvas damage.
2. **Source of truth:** `kwm_windows[]` under `kwm_lock` owns content dimensions,
   frame position, canvas, z-order and validated opacity hint. Titlebar is outside
   the content canvas; desktop has no titlebar. Move snapshots old/new positions.
   `g_screen_dirty` under `g_dirty_lock` is final **screen-space scene damage**.
3. **Accumulation:** `DirtyRegionList` holds 64 rectangles. Containment drops
   redundant rectangles; beneficial overlaps merge; capacity pressure merges the
   least-cost pair. No intentional full-screen collapse. Screen clipping happens
   too late (after accumulation), permitting overflowing unions of extreme input.
4. **Composition:** damage is snapshotted and cleared, then clipped to the screen.
   Each region rebuilds base + windows low-to-high + software cursor. Opaque
   content subtracts base/lower-window work; bounds exhaustion falls back safely.
   Output is `backbuffer`, with stride from `display_get_mode()`.
5. **GHAL:** each composed rectangle uploads from full-stride backbuffer into
   the same coordinates in the scanout surface. Up to four rectangles present
   separately; more become one bounding box, potentially spanning the screen.
6. **VirtIO:** backing is linear, stride `surface.width * 4`; transfer offset is
   correctly `y * stride + x * 4`. TRANSFER and FLUSH are separate fenced chains.
   Upload rectangles and presentation rectangles are distinct, despite sharing
   screen coordinates for this scanout. CPU upload is not a DMA transfer.
7. **Software:** scanout wraps firmware framebuffer, preserving padded pitch.
   Upload copies directly to scanout; present is a no-op. Private surfaces copy
   their clipped present rectangle to framebuffer.
8. **Synchronization defects:** compositor callers include timer and printing
   paths, but only composition is locked, not the shared backbuffer/present
   transaction. Fence wait timeout is ignored before backing is overwritten.
   VirtIO submission may skip/fail after damage was cleared. Used-ring polling
   runs outside the submit lock; a batch fence is retired by its first chain,
   before FLUSH completes. CPU/device ring publication needs explicit ordering.
9. **Other findings:** opacity is validated once, never revalidated after content
   writes, and its validation reads an unlocked canvas. Close-button click exits
   before focus/z invalidation. Mouse bring-to-front omits z normalization;
   creation normalizes before marking the new slot active. New canvases are
   uninitialized. Software cursor is invalidated even when stationary.

Move already damages both visual bounds, including the shadow's +3px offset.
Destroy already damages the old bounds. These features do not need replacement.

## Existing lifecycle limits

KWM exposes create/update/destroy/activate and mouse drag. There is **no kernel
resize, minimize/restore, or hide/show API/state**; `kwm_window_canvas_bytes`
explicitly documents fixed size. Tests must label synthetic geometry/visibility
transitions separately from real public operations. Adding a new window ABI is
outside this repair.

Canvas high byte is a binary opacity mask (0 transparent, nonzero opaque), not
fractional alpha. Fractional-alpha images are blended into an opaque UI canvas.
Framebuffer X byte is unused; differential comparisons compare displayed RGB.

## Debug workflow

`KWM_DEBUG_FLAGS` accepts `-DKWM_DEBUG_FULL_REDRAW=1`,
`-DKWM_DEBUG_DISABLE_OPAQUE_OPT=1`, `-DKWM_DEBUG_DAMAGE=1`, and
`-DKWM_DEBUG_RECTS=1`. Defaults are zero. Use separate `BUILD_DIR` values when
changing flags so make cannot reuse objects built with other settings.

Diagnostics report screen/composition/upload/present pixel work separately.
Rectangle pixel counts sum submitted areas (including overlapping work).
VirtIO transfer area equals present area; software upload writes scanout directly.
`base_pixels` and `content_pixels` expose layer work hidden by output-area counts.
TSC is calibrated once against PIT2 in debug boot; failed calibration reports
`clock_valid=0`. Serial logging is outside measured CPU frame time and can itself
slow a debug build. `frame_time_us` is CPU submission time, not scanout latency.

`make test-damage` runs real display/KWM/compositor code and a fresh full-scene
reference for every operation, including padded stride and masked transparency.
`make test-damage-driver` runs the real VirtIO backend/transport against a RAM
device with out-of-order completions and queue-lock contention.

## Fixes (2026-09-24)

Lifecycle/opacity/clipping (`kernel/gfx/kwm.c`, `kernel/gfx/compositor.c`):

- New canvases are zeroed; extreme frame coordinates are rejected.
- `z_index` normalization runs after the new slot is active (create) and on
  mouse bring-to-front; the duplicate normalize call in activate is gone.
- Full and partial uploads revalidate the opacity hint under `kwm_lock`;
  declaration scans under the same lock (no unlocked canvas read).
- Close-button click invalidates focus/z-order before routing the event.
- Close glyph and hover fill clip to the titlebar intersection, not the damage
  rect (fixes overdraw on narrow windows).
- `screen_mark_dirty` clips to the mode before coalescing.

Synchronization/submit (`kernel/gfx/compositor.c`, `graphics/ghal.*`,
`graphics/backend/virtio_gpu.c`, `drivers/graphics/hw/virtio_gpu_dev.*`,
`drivers/graphics/hw/virtqueue.c`):

- One `g_compositor_lock` owns composition through upload; reentrant/nested
  flushes defer instead of tearing the backbuffer. `kwm_lock` is try-locked;
  busy scenes retain damage via `screen_restore_dirty`.
- Damage is consumed only after the previous frame's fence retires (poll, never
  a wait in the timer IRQ). Failed/partial submits retain the rejected suffix;
  device errors restore accepted inflight via the error counter.
- New `ghal_present_checked` reports submission; legacy void present preserved.
- Fence is a contiguous watermark over both chains of every batch, whatever the
  used-ring order; response slots are owned per async pair, not round-robin.
- Queue reserve/reap/publish holds one try-lock; polling never reaps under
  another owner. Ring publication uses release/acquire fences; used-ring heads
  are validated before reclaim. `virtio_blit` and both upload paths clip the
  source rectangle as well as the destination.
- New scanout surface starts fully dirty (firmware ink is not the scene).

Present work (`kernel/gfx/compositor.c`, `kernel/gfx/kwm.c`):

- Union present only when the bounding box is within 2x the summed rect areas;
  scattered rects present per-rect (queue pressure retries safely). Six far
  apart 1px updates: present 63717 px -> 198 px (322x), 7 rects.
- Multi-window destroy marks one rect per window instead of a spanning bbox.

## Measurements (host, 320x240, stride 336)

- `test-damage` all modes: pixel_mismatches=0 (partial, full, partial-no-opaque,
  full-no-opaque, async-model). Full redraw is the oracle, never the path.
- Lifecycle/stress checks: 5120-rectangle coalescing coverage, 16-slot
  pressure, extreme-coordinate clipping, reentrant flush, producer-during-
  present, busy-lock retention — all pass.
- Sparse frame: damage=composite=upload=present=198 px, 7 rects.
- Replay averages (partial, opaque on): damage/composite/upload ~16k px,
  present ~16.5-17.5k px; CPU submit ~15-25 us avg (host TSC, qualitative).
- QEMU boot (fixed kernel): VirtIO 1920x1080 login + desktop + wallpaper, no
  device errors; software fallback (`-vga std`) login + desktop + wallpaper.
  One std-VGA boot stalled once at module 10/74 and passed on retry; baseline
  and fixed kernels both pass std-VGA, attributed to TCG/ATA flake, not the
  diff (stall is in KFS module install, untouched by this change).
