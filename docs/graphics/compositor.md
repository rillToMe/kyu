# Compositor

The compositor (`kernel/gfx/compositor.c`) builds each frame from a background
layer plus windows and presents only the changed regions. It is designed to
avoid flicker and to minimize both CPU work and GPU traffic.

## Buffers

| Buffer | Global | Role |
| --- | --- | --- |
| Base canvas | `base_canvas` | Persistent background (TTY, HUD, wallpaper) |
| Back buffer | `backbuffer` | Rebuilt each frame from base + windows |
| Hardware framebuffer | `fb_ptr` | The actual scanout memory |
| Main scanout surface | `g_main_surface` | GHAL surface wrapping the scanout |

Both screen buffers are sized `pitch_bytes * height` (pitch-aware, not
`width * height`). They are allocated in `display_alloc_buffers()`. The scanout
surface is created once in `compositor_ghal_init()` in task context (not IRQ
context, because `kmalloc` and virtqueue setup are not IRQ-safe).

## Dirty-Region Tracking

Damage is tracked in a fixed list of rectangles.

| Constant | Value |
| --- | --- |
| `MAX_DIRTY_REGIONS` | 64 |

`screen_mark_dirty(x, y, w, h)` clips the rectangle to the display mode before
coalescing (unions of unbounded origins cannot fit a `Rect`, and off-screen
gaps are not damage), then merges it into the list under `g_dirty_lock`.

### Coalescing rules

1. If any existing region **contains** the new one, drop it (no-op).
2. Otherwise merge into the overlapping region with the **lowest**
   `merge_inflation`, but only when that cost is ≤ 0; then drop any contained
   regions.
3. Otherwise append, while the list has fewer than 64 entries, dropping any
   regions the new one fully covers.
4. If the list is full, do **not** collapse to a single bounding box. Instead
   compare the cheapest single-region growth against the cheapest existing pair
   merge and take the lower cost.

`merge_inflation(a, b)` is the signed 64-bit difference
`bbox_area(union(a,b)) - area(a) - area(b)`. Overlapping rectangles yield a
negative cost (a strict win); separated ones yield a positive cost (wasted
gap).

**Invariant:** the union of coalesced regions always **covers** the union of
requested regions. Over-invalidation is allowed; under-invalidation is not.

## Frame Flush

`compositor_flush()` is the entry point. It takes `g_compositor_lock` with
`spinlock_try_lock` and returns immediately if the lock is held (a timer
interrupt can interrupt a print-triggered flush; spinning on that owner would
deadlock).

The flush sequence:

1. **Panic lockdown** — if the panic state is locked, hide the cursor and
   return.
2. Read the display mode; snapshot and clear the dirty list under
   `g_dirty_lock`.
3. **Fence gate (backpressure)** — if the previous frame's fence is still
   pending, defer and return without consuming damage or touching the backing
   store. If the previous frame produced a device error on an unverified
   in-flight region, restore that region's damage.
4. **Cursor folding** — mark the old and new cursor rectangles (software
   cursor) or move the hardware cursor.
5. Acquire `kwm_lock` with `try_lock`; on failure, restore the damage and
   defer.
6. Per region: intersect with the screen, base-blit, composite windows.
7. Present.

## Occlusion Culling

Inside `composite_windows_in_rect`, fully opaque windows act as occluders. For
each window, content is drawn only over the part of the clip not covered by the
content of any opaque window in front of it.

| Constant | Value |
| --- | --- |
| `KWM_MAX_OCCLUSION_RECTS` | 8 |
| Occluder collection size | 16 |

`rect_subtract(R, C, out, ...)` emits up to four disjoint strips (top, bottom,
left, right) that exactly partition `R` minus `C`. If the occluder list
overflows, the code falls back to the full clip (always safe). Only the
**content** draw is culled; shadows and title bars still render.

## Base-Blit Elimination

`base_blit_for_region` avoids copying the background underneath opaque windows.

| Constant | Value |
| --- | --- |
| `KWM_SPLIT_MIN_PIXELS` | 1024 |
| `KWM_MAX_OPAQUE_SPLITS` | 4 |
| `KWM_MAX_SPLIT_RECTS` | 12 |

Algorithm: start with `remain = {r}`. Repeatedly pick the opaque window whose
content covers the most of `remain` (greedy), stop if no window intersects or
the cover is below 1024 px and does not fully remove a rectangle, and subtract
the cover from every remaining rectangle. Base-blit each leftover rectangle.

Composition still runs **exactly once** over the full region; only the base
copy is decomposed. Any bounds failure causes **more** base blitting, which is
always safe.

## Opaque Fast Path

In `mix_content`, an opaque window is blitted with `memcpy`; otherwise each
pixel is tested (`pixel >> 24`) and copied with its alpha mask applied.

`fully_opaque` is a **kernel-validated performance hint**: it is set only by
`kwm_set_window_opaque()` after scanning every canvas pixel (any transparent
pixel rejects it) and is cleared on create/free. A `FORCE_SCALAR_COMPOSITOR`
compile switch forces the non-fast path for comparison.

## Present

Dirty regions are uploaded to the scanout surface and presented.

| Constant | Value |
| --- | --- |
| `PRESENT_UNION_MAX_INFLATION` | 2 |

If there is more than one dirty rectangle, the compositor computes their union
and the sum of their areas and presents a **single** union rectangle only when
`union_area <= sum * 2` (i.e., the rectangles are neighbors). Scattered
rectangles are presented individually. This dramatically reduces present
traffic for sparse updates.

If a present is rejected (`present_checked != 0`), the rejected suffix of
rectangles is re-marked dirty. For backends with `GHAL_CAP_ASYNC_PRESENT`, each
presented rectangle is recorded in `g_inflight_dirty` and a fence is captured.

If no GHAL backend is active, the compositor falls back to a direct
framebuffer copy (`blit_rect_db`).

## Cursor

| Constant | Value |
| --- | --- |
| `CURSOR_WIDTH` | 12 |
| `CURSOR_HEIGHT` | 16 |
| `HW_CURSOR_SIZE` | 64 |

Cursor shapes: arrow (0), I-beam (1), hand (2), selected via `kwm_set_cursor`
(syscall 58). A software cursor is drawn into the back buffer, with both the
old and new cells marked dirty. A hardware cursor uses a 64×64 ARGB scratch
surface and is only enabled if the backend advertises `GHAL_CAP_HW_CURSOR`;
on any failure it rolls back to software.

## Debug Counters

`kernel/gfx/damage_debug.h` provides compile-time flags (all default 0) and
counters: `damage_rect_count`, `damage_pixel_count`, `composite_pixel_count`,
`base_pixel_count`, `upload_pixel_count`, `present_rect_count`, `deferred`,
and `frame_time_us` (TSC calibrated against PIT channel 2).

Flags: `KWM_DEBUG_FULL_REDRAW`, `KWM_DEBUG_DISABLE_OPAQUE_OPT`,
`KWM_DEBUG_DAMAGE`, `KWM_DEBUG_RECTS`.

## Related Documentation

- [Graphics Overview](README.md)
- [Window Manager](window-manager.md)
- [Backends](backends.md)
- [Display & Primitives](display.md)
