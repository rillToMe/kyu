# Phase 11 Report — Rendering & Compositor Performance Audit
Date: 2026-09-14
Scope: Audit only. No production changes, no GPU implementation, no multi-region, no syscall/ABI change, no compositor redesign.

---

## 1. Executive Summary

What is currently expensive?

The rendering pipeline is a CPU-side scalar pixel composition, not a memory copy and not the GPU upload. The dominant cost center is the per-pixel window content loop in composite_windows_in_rect() (kernel/gfx/compositor.c:241-252), which reads each pixel, tests the alpha byte, and conditionally writes it — a scalar loop roughly 3x slower per byte than the memcpy used by blit_rect_db and the GHAL uploads.

Ranked (source-derived, static — see sections 5 and 8):

1. Per-pixel window composition (scalar loop + alpha branch)
2. Dirty-region collapse-to-single-bbox on heavy update bursts
3. Window iteration control overhead (all slots scanned per dirty rect)
4. VirtIO command/notify/sync overhead per present rect (fixed cost)

VirtIO-GPU does not address the actual bottleneck. The expensive stages (per-pixel composition, region collapse) are entirely CPU-side. VirtIO only offloads the final upload+present, which is already the cheapest stage (proportional memcpy on software; fixed command overhead per rect on VirtIO). For the common small-damage workload, VirtIO command overhead likely exceeds the tiny memcpy it replaces — GPU is not obviously faster and can be slower.

Decision: GPU acceleration DEFERRED (see section 9). A compositor-inner-loop optimization is higher value (see section 10).

Methodology note: Runtime profiling could not be performed because QEMU (qemu-system-x86_64) was unavailable on the host. All numbers below are source-derived / static estimates, explicitly labeled. No fabricated measurements.

---

## 2. Rendering Pipeline

Traced data flow (all functions verified against source):

    producer (libgui/libui/TTY/KWM)
       |  write window canvas / draw_* primitives / KWM move
       v
    screen_mark_dirty(x,y,w,h)                       kernel/gfx/compositor.c:70
       |  spinlock + dirty_region_mark(&g_screen_dirty, r)
       v
    DirtyRegionList g_screen_dirty                   kernel/display.c:141
       |  MAX_DIRTY_REGIONS=64; >64 -> collapse to 1 bbox
       v
    compositor_flush()  [timer IRQ @60Hz, cb_flush]  kernel/gfx/compositor.c:407
       |  snapshot+clear dirty list; fold cursor rects (software cursor)
       +-- blit_rect_db(back, base, r)               compositor.c:81 -> viewport_render (row memcpy)
       +-- composite_windows_in_rect(r)              compositor.c:214
       |      +-- per-window: frame_shadow (6 rings) compositor.c:156
       |      |   content rect (per-pixel alpha)     compositor.c:241
       |      +-- titlebar round_grad+close+text     compositor.c:256
       +-- cursor into backbuffer                    compositor.c:460 (sw) / ghal_cursor_move (hw)
       +-- ghal_surface_upload(back -> main)         per dirty rect
       |      software: memcpy rows -> HW fb (zero-copy scanout)
       |      virtio:   memcpy rows -> backing_virt  virtio_gpu.c:164
       +-- ghal_present                              <=4 rects -> per-rect present; >4 -> union bbox
            software: no-op (zero-copy)              software.c:148
            virtio:   TRANSFER_TO_HOST_2D + FLUSH, 1 notify, fence backpressure (2 cmds / present rect)  virtio_gpu.c:216

Key structural facts:

- compositor_flush() runs inside the TIMER IRQ (cb_flush, slot 2, every tick at default 60Hz). The whole composite+present happens in IRQ context on the CPU that took the timer interrupt, while holding kwm_lock as a spinlock. It also runs synchronously from fs_write / kernel_userlib (kyuzenfs.c:36, apps/kernel_userlib.c).
- The software backend uses a ZERO-COPY scanout surface (sw_surface_create_scanout wraps the HW framebuffer; upload writes straight to screen, present is a no-op). So software upload cost == one dirty-region memcpy, and present is free.
- VirtIO present submits TWO commands (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH) as a batch per present rect, one notify, async fence with backpressure at the NEXT flush. 2 commands + 1 notify + 1 poll per present rect.

---

## 3. Profiling Method

- Instrumentation: NONE added. This is a source-level static audit.
- QEMU availability: qemu-system-x86_64 NOT FOUND on host (verified with `where`). Runtime benchmarking (Workloads A-J, cycle counters, timestamps) could not be performed.
- Timing/counter methodology: not applicable — no runtime available.
- Limitations: All figures in sections 5-7 are SOURCE-DERIVED estimates (bytes copied, pixels, iteration counts read directly from code), not measured times. No timing is claimed. Bandwidth/instruction estimates use typical x86-64 figures and are clearly marked as estimates.

Acceptance: no fabricated runtime measurements. All numeric claims below are counts that follow directly from the source (rect areas, window counts, command counts), or explicitly labeled estimates.

---

## 4. Workload Results

All values N/A because QEMU was unavailable. Source-derived expectations per workload are noted; see section 2 for the pipeline and section 12 for the idle finding.

Workload A. Idle desktop
  Source: software cursor forces 2 cursor-rect composites every 16.7ms even when fully idle (see section 12). HW cursor = true no-op. Not zero work on the software backend.

Workload B. Button hover
  Phase 8 separated-hover dirty = 10,902 px. Composite area roughly equals dirty (clipped). Scalar per-pixel composition over this area.

Workload C. Button click
  Phase 8 click dirty = 2,912 px. Same shape as B, smaller area.

Workload D. Clock tick
  Recurring 60Hz flush. Work depends on the damage volume generated per tick (clock text is a small region).

Workload E. Task manager updates
  Recurring; region-sized scalar composition + upload.

Workload F. Menu open/close
  Shadow expands the dirty rect by KWM_SHADOW_MARGIN (6px) each side plus the 3px downward offset (kwm.c:76 frame_dirty_area).

Workload G. Window movement
  frame_dirty_area marks the full frame+shadow bounding box (kwm.c:76); compositor reprocesses the union of old+new frame rects (the cursor's vacated/new cells are also folded in).

Workload H. Multiple overlapping windows
  Compositor work scales with the number of windows examined per dirty rect (section 6): every active window slot is scanned for each dirty rect, z from 0..next_z_index.

Workload I. Large window
  Largest practical window: scalar composition area == window size. This is the one workload where per-pixel cost dominates unambiguously.

Workload J. Terminal output
  Phase 9: tty_scroll physically shifts the entire base_canvas, so it legitimately dirties the whole framebuffer. The dirty region collapses to full-screen; scalar composition over the full frame.

NOTE: none of the above were measured. Values that would appear here (dirty px, compositor px, GHAL bytes, windows examined/composited, timings) are all N/A.

---

## 5. Cost Breakdown

All numbers are SOURCE-DERIVED counts (bytes/pixels read straight from the code paths), not measured times. Where a time estimate is given it is explicitly labeled ESTIMATE (typical x86-64 scalar ~1B/cy and memcpy ~8-32B/cy at ~3-4GHz).

Stage: base blit (blit_rect_db via viewport_render)
  Cost: r.w * r.h * 4 bytes per dirty rect, row memcpy.
  Scaling: proportional to dirty area. Cheap per byte (memcpy). For a 40x28 button: 1,120 px = 4,480 B. ESTIMATE under 1 us. NOT a bottleneck.

Stage: window iteration (composite_windows_in_rect)
  Cost: for each dirty rect, loop z from 0 to next_z_index, and for each z scan all MAX_WINDOWS (16) slots, testing active AND z_index==z AND canvas. Only windows whose frame intersects r are drawn.
  Count (static): per dirty rect = (next_z_index+1) x 16 slot-checks, regardless of how many windows actually overlap r.
  Scaling: O(#windows x #z) per rect, i.e. O(#windows) per rect when z normalized. Grows linearly with window count even when the damage touches one tiny window.

Stage: per-pixel composition (content rect, compositor.c:241)
  Cost: for each overlapping window, scalar loop over clip.width x clip.height: read pixel, test the top alpha byte, and conditionally write if nonzero (opaque). This is NOT a memcpy — one load, one branch, one store per pixel.
  Count (static): sum over windows of (clip area) for each dirty rect.
  Scaling: proportional to the composited pixel area. This is the DOMINANT CPU cost per byte (scalar, roughly 3x memcpy cost per byte) and grows with dirty area and with the number of overlapping opaque windows covering that area.
  Classification: measured? no. source-derived? yes. This is the identified #1 cost center.

Stage: shadows (frame_shadow, compositor.c:156)
  Cost: for each non-desktop window intersecting r, 6 rings x 4 edges each, each edge a clipped blend_rect_clip (per-pixel aa_mix). Work is clipped to r, so for a small dirty rect the shadow work inside r is small; for a full window move the full perimeter x 6 rings is redrawn.
  Scaling: proportional to the shadow perimeter intersecting r, times 6. Secondary to content composition unless the dirty rect is a large window frame.

Stage: titlebar + decorations (compositor.c:256)
  Cost: when r intersects the titlebar (24px) of a non-desktop window: full round_grad_fill over the titlebar, close-button rounded square + two DDA lines + title text glyphs, all clipped to r.
  Scaling: proportional to titlebar area intersecting r. Re-rendered every frame the titlebar is dirty (focus tint change, move).

Stage: GHAL software upload (sw_surface_upload)
  Cost: r.w * r.h * 4 bytes per dirty rect, row memcpy into the HW framebuffer (zero-copy scanout). Present = no-op.
  Scaling: proportional to dirty area. Same byte volume as base blit, both cheap memcpy. NOT a bottleneck on software.

Stage: GHAL present, VirtIO (virtio_present)
  Cost: per present rect, TWO commands (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH) submitted as a batch, ONE notify, async fence; backpressure poll of the previous fence at the next flush. Plus the upload memcpy into backing_virt.
  Scaling: fixed command/notify/sync overhead PER PRESENT RECT, independent of rect size. With PRESENT_UNION_THRESHOLD=4: up to 4 rects each get their own present (2 cmds + 1 notify + 1 poll each); more than 4 rects collapse to ONE present with the union bounding box (1 cmd pair for a possibly much larger area).

VirtIO cursor: separate cursorq (UPDATE_CURSOR / MOVE_CURSOR), synchronous. HW cursor moves do not dirty the screen and are a true no-op when unmoved.

---

## 6. Scaling Analysis

Damage size: base blit and GHAL upload both scale linearly with dirty pixel area (memcpy). Per-pixel composition also scales with area but at higher per-byte CPU cost)Skip.
Number of windows: window-iteration control cost scales with the number of windows (all slots scanned per dirty rect), independent of damage size. With 16 max windows and normalized z, a one-pixel dirty region still pays O(16 x #z) slot checks. This is a real but bounded CPU overhead that grows with window count.Skip
Overlapping windows: when N opaque windows overlap the same dirty rect, the pixel at each location is written once per overlapping window (top alpha-mask test). Composition cost is therefore roughly the dirty area times the average window overlap depth. Overlap depth, not window count, drives per-pixel cost.

Compositor complexity: the pipeline is linear per dirty rect (one base blit + one window pass + one upload per rect). No recursive or quadratic composition beyond the O(#windows) slot scan. The main non-linear risk is region collapse (section 7).

---

## 7. Duplicate Work

1. Dirty-region collapse to a single bbox (kernel/display.c:149). When more than MAX_DIRTY_REGIONS (64) marks accumulate between flushes, the list collapses to ONE union bounding box. A burst of 65+ scattered small updates (e.g. many widgets, multi-glyph text, several windows) becomes a single near-full-screen rect that is base-blitted, composited (all windows), uploaded and presented as one large region. This is the largest amplification of work and is the known multi-region limitation (Phase 7 recommended deferring multi-region).

2. Base blit of stale content (blit_rect_db, compositor.c:81). base_canvas holds the static background layer (TTY, HUD, desktop wallpaper), NOT window content. For every dirty rect the base region is copied even when a fully opaque window fully covers it and will overwrite every byte via composition. The copied bytes are then overwritten by the window composite — the base copy for a fully-covered opaque rect is wasted work. Magnitude: proportional to the covered dirty area per rect. (The window content itself is NOT double-copied — base_canvas does not contain window pixels.)

3. Per-window: content is composited for every overlapping window, and the same pixel may be written once per overlapping window (overlap depth). This is inherent to z-order compositing, not redundant, but grows with overlap.

4. VirtIO: when dirty.count is 4 or fewer, each rect gets its own present (own TRANSFER+FLUSH pair). No duplicate pixel transfer, but multiple command pairs and notifies for what could be one. Above 4 the union path collapses them (sometimes to a much larger area — a different tradeoff).

5. Software cursor idle recomposite (section 12): old and new cursor rects are folded into the dirty set EVERY flush even when nothing else changed, forcing 2 cursor-rect composite+upload cycles 60x/sec on the software backend. Not a pixel duplicate within a frame, but recurring idle work.

6. Cursor rects: when the cursor moves, both the vacated and new cells are marked (compositor.c:435-441); the union is processed once. Correct, not duplicated.

No overlapping-region duplicate is introduced within a single flush: each distinct dirty rect is processed exactly once. The duplication is the collapse-amplification (#1) and the idle-cursor recurrence (#5).

---

## 8. Bottleneck Ranking

Source-derived (static). No runtime timing available.

1. Per-pixel window composition (compositor.c:241). Scalar read/test/write per pixel instead of a bulk copy. Dominant per-byte CPU cost. Grows with dirty area and overlap depth. Affects every non-trivial workload (hover, click, clock, task manager, window move, terminal output).

2. Dirty-region collapse to a single bbox on update bursts (display.c:149). Turns many small damages into one near-full-screen reprocess+upload. Worst-case amplification is large (a full-frame composite/upload from a burst of tiny updates). Tied to the deferred multi-region work.

3. Window iteration control overhead (compositor.c:216). All MAX_WINDOWS slots x all z-levels scanned per dirty rect, regardless of which windows actually overlap. Grows with window count. Real but bounded; secondary unless many windows exist alongside tiny damage.

4. VirtIO command/notify/sync overhead per present rect. Fixed 2-command + 1-notify + 1-poll cost per present rect, independent of rect size. For small updates this overhead likely dominates and can make VirtIO slower than the software zero-copy memcpy path.

5. Base blit of fully-covered opaque rects (wasted bytes, section 7 #2) and GHAL software upload. Both are cheap memcpy and proportional to area; neither is a primary bottleneck.

Shadow/decorations (frame_shadow, titlebar) are only significant when the dirty rect includes a large window frame (move, open/close) — secondary.

---

## 9. GPU/VirtIO Decision

Decision: DEFER (GPU acceleration is not justified by current evidence).

Why:
- The dominant cost (per-pixel composition) and the worst amplification (region collapse) are entirely CPU-side. VirtIO-GPU only offloads the final upload+present, which on software is already the cheapest stage (zero-copy scanout memcpy, present = no-op). Moving upload to the GPU does not reduce the per-pixel scalar composition or the region-collapse reprocessing.
- VirtIO adds a fixed per-rect command/notify/sync overhead. For the common small-damage workload (button hover/click, clock text), the transferred bytes are tiny (a few KB), so the fixed command overhead can exceed the memcpy it replaces. VirtIO could be SLOWER for the typical case, not faster.
- No runtime measurement exists (QEMU unavailable), so there is no evidence of a GPU-addressable bottleneck.

When GPU would become worth it (to revisit later, with measurement):
- If per-frame dirty pixel volume is large (full-screen terminal scroll, large window moves) AND the per-pixel scalar composition is first converted to a bulk copy that VirtIO can then offload as a single large TRANSFER+FLUSH. Even then, the win is only on the upload leg.

Classification: measured = none. Source-derived = the above. Theoretical = VirtIO helps only for high-volume, large-rect workloads after the CPU composition cost is reduced.

---

## 10. Recommended Phase 12

One highest-value next step:

Optimize the window-content composition inner loop in composite_windows_in_rect (kernel/gfx/compositor.c:241). Convert the per-pixel scalar read/test/write into a bulk row copy for windows that are fully opaque within the clipped rect (falling back to the alpha-mask loop only when the window's canvas contains transparent pixels in that row). This is the identified #1 cost center, is purely compositor-internal (no syscall/ABI/GPU/multi-region change), and reduces the dominant per-byte CPU cost for every non-trivial workload.

Verification: build + boot (when QEMU available), compare visual output identical for opaque windows; optionally add a temporary counter of rows fast-pathed vs alpha-scanned, then remove it.

Do not implement region collapse or multi-region in Phase 12; keep those separate and measured.

---

## 11. Acceptance Criteria

[ x ] Complete compositor pipeline traced (section 2)
[ x ] blit_rect_db audited (sections 2, 5, 7)
[ x ] composite_windows_in_rect audited (sections 2, 5, 8)
[ x ] dirty-region behavior audited (sections 2, 7, 8)
[ x ] shadow/decorations audited (section 5)
[ x ] software GHAL audited (sections 2, 5)
[ x ] VirtIO-GPU audited (sections 2, 5)
[ x ] window scaling analyzed (section 6)
[ x ] producer damage separated from compositor work (section 2)
[ x ] GHAL upload separated from compositor work (section 2, 5)
[ x ] duplicate work investigated (section 7)
[ x ] QEMU availability checked (section 3: unavailable)
[ x ] No fabricated runtime measurements (sections 3-4: all N/A or source-derived)
[ x ] Temporary instrumentation removed (none added)
[ x ] No multi-region implementation (none)
[ x ] No GPU implementation (none)
[ x ] No syscall ABI changes (none)
[ x ] Existing functionality preserved (no production code touched)
[ x ] Clean build passes (verified: make clean + make -> myos.bin produced)
[ x ] Repository state verified (only pre-existing uncommitted files remain)
