# Phase 16 — Opaque Window Base-Blit Elimination

Date: 2026-09-14
Status: COMPLETE — BASE BLIT ELIMINATION VALIDATED

---

## Status

Production code changed in one kernel file (kernel/gfx/compositor.c). For each
dirty region the compositor now skips the base_canvas -> backbuffer copy when a
visible, validated fully-opaque window's content area completely covers that
region. A host harness exercises the REAL compositor_flush() and compares it to
a forced-base-blit baseline: rgb_diff == 0 and no uninitialized pixel left in
all eight coverage cases. Kernel, Rust apps and the hybrid ISO build; QEMU boots
and still responds to keyboard and mouse with no panic.

---

## Objective

Remove the Phase 11 duplicate-work item #2: for a dirty rect fully overwritten
by an opaque window, the base copy is dead work (it is immediately overwritten
by composition). Skip it per dirty rect, keep it everywhere else.

---

## Existing Base-Blit Path

compositor_flush() (kernel/gfx/compositor.c), before this phase:

    for each coalesced dirty region (Phase 15):
        r = intersect(region, screen)
        blit_rect_db(back_db, screen_db, r)      <-- base_canvas -> backbuffer
        composite_windows_in_rect(r, pitch4)
    ...cursor, then GHAL upload/present...

blit_rect_db() wraps viewport_render() (kernel/display.c) and copies rows.
composite_windows_in_rect() then draws, per window in z-order, the shadow
frame, the content canvas (opaque memcpy fast-path from Phase 14, else scalar
alpha), and the WM titlebar, all clipped to r.

base_canvas is the persistent background layer (TTY output, HUD, wallpaper),
written independently of windows. backbuffer is rebuilt each frame from it and
from the windows. The base copy is only needed for pixels that no window writes.

---

## Coverage Condition

The base blit for region r may be skipped iff:

    there exists an active, composited window W with fully_opaque == 1
    whose CONTENT area completely contains r.

Formally r is a subset of W's content rect, intersected with the screen.
Only the window CONTENT canvas counts — never the frame, titlebar or shadow.

Why this is sufficient (the safety proof): when W is composited over r, its
content loop is clipped to r and, because W passed the Phase 14 kernel opacity
validation (every canvas pixel has a non-zero alpha byte), it WRITES EVERY PIXEL
of r. Windows below W are drawn first and overwritten; windows above W only draw
on top, and their transparent pixels leave the opaque layer showing. Cursor
pixels with value 0 also leave the opaque layer. Therefore no pixel of r depends
on base_canvas, and the copy cannot affect the final frame.

This matches the spec: "one opaque window covering the COMPLETE dirty rect is
sufficient."

Any uncertainty returns false and keeps the base blit. A false negative costs
performance; a false positive would corrupt the frame.

---

## Z-Order Handling

Z-order is respected and unchanged. The helper does NOT require the opaque
window to be topmost, because the proof above is z-order independent: an opaque
write to every pixel of r is enough, and later writes never "unwrite" a pixel.
No window ordering, frame, shadow, titlebar, cursor or GHAL behaviour changes.
No occlusion culling is performed.

---

## Implementation

kernel/gfx/compositor.c, two new static helpers plus a guarded call:

- rect_covers(outer, inner): 64-bit-safe containment.
- rect_covered_by_opaque_window(r): scans the bounded window slots
  (MAX_WINDOWS = 16) for an active window with a canvas, fully_opaque == 1,
  whose content rect (x, y + titlebar, width, height; titlebar = 0 for the
  frameless desktop) contains r. Mirrors composite_windows_in_rect's geometry.
- compositor_flush(): the loop now reads

      if (!rect_covered_by_opaque_window(r))
          blit_rect_db(back_db, screen_db, r);
      composite_windows_in_rect(r, pitch4);

The hardware-framebuffer copy in the no-GHAL fallback branch is a DIFFERENT blit
(backbuffer -> HW framebuffer = present) and is never skipped.

No new syscalls, no new metadata, no allocation, bounded O(MAX_WINDOWS) per
region.

---

## Correctness Validation

Harness (OS temp dir) compiles the REAL kernel/gfx/compositor.c (plus
kernel/display.c) for the host. Because spinlock_lock_irqsave executes cli/sti
(privileged), the lock helpers are macro-mocked to no-ops; nothing else is
altered. The harness calls the real compositor_flush() for the optimized path
and a reconstructed forced-base-blit loop for the baseline, with backbuffer
pre-filled with a sentinel (0x00DEAD00) so any un-written pixel is detectable.
Pixels are compared on RGB (top/X byte ignored, as documented in Phase 14).

For each case it reports: helper skip decision, regions, rgb_diff over the whole
framebuffer, sentinel pixels left inside dirty regions, and base bytes that
flush would skip.

---

## Host Tests

MEASURED (host harness, real compositor_flush):

    Case  Scene                                  helper  regions rgb_diff sentinel skip
    A     dirty 100x100 inside opaque 200x200     skip     1       0        0      40000/40000
    B     dirty 50x50 inside opaque 300x250       skip     1       0        0      10000/10000
    C     dirty 100x100 vs opaque 50x50 (partial)  keep    1       0        0          0/40000
    D     intersecting, not covering               keep    1       0        0          0/36000
    E     fully covering but NON-opaque window     keep    1       0        0          0/40000
    F     opaque below, non-opaque above overlaps  skip    1       0        0      40000/40000
    G     opaque window partially off-screen edge  skip    1       0        0      25600/25600
    H     two dirty regions (one covered, one not) mixed   2       0        0       9600/16000

    rgb_diff == 0 in every case; zero sentinel pixels left; helper decisions match
    expectations; per-region decisions are independent (case H).

Interpretation:
- Cases A/B/F/G skip the base copy and still produce a pixel-identical frame
  with no uninitialized pixel (the opaque window writes all of r).
- Cases C/D/E correctly keep the base blit (partial coverage, intersecting-only,
  or non-opaque), so no unsafe skip.
- Case F confirms the z-order reasoning (opaque under a partially-covering
  non-opaque window is still sufficient).
- Case G confirms off-screen clipping: only the on-screen part of the content
  matters and the dirty rect is inside it.

---

## QEMU Tests

QEMU 10.2.1 (E:\Tools\msys2\mingw64\bin). Hybrid ISO booted with -display none,
serial to file, monitor on TCP.

- No panic / exception / fault (serial clean).
- Login screen renders (1280x800, 1,023,939 non-black pixels).
- sendkey "help" changed the framebuffer (CRC changed) — live incremental
  damage + compositor path works.
- mouse_move changed the framebuffer (CRC changed) — cursor dirty rects work.
- No stale pixels, trails or corruption observed.

Limitation: launching a Rust/Slint window (the workload that triggers the skip)
requires logging into the image and running an app; the persisted password on
disk.img is unknown, so automated app launch was not performed. The opaque-skip
path is therefore validated by the host harness against the real
compositor_flush rather than by an on-screen QEMU window. QEMU confirms the
integration is regression-free.

---

## Performance Measurements

MEASURED (host harness, real helper decisions, real flush):
- Base bytes skipped per scene are listed above (e.g. 40,000 B for a 100x100
  region fully covered; 9,600/16,000 B in the mixed case H).
- elision ratio equals covered_dirty_bytes / total_dirty_bytes.

INFERRED (source-derived) for real workloads:
- A Rust/Slint window is the only current fully_opaque producer (Phase 14).
  Any dirty region inside that window's content (button hover, click, partial
  redraw, cursor moving over the window, clock/taskbar areas owned by the
  window) is fully covered -> base blit skipped. This is the common case for
  window-internal updates.
- Regions that span window edges (window move: old+new frame bounds), regions
  over the desktop/TTY, or regions over libgui/libui windows are not covered
  -> base blit kept, unchanged behaviour.

NOT MEASURED / NOT CLAIMED:
- No CPU-time speedup figure. No per-frame counters were added to the kernel
  (temporary instrumentation must be removed; none was added). The byte-copy
  reduction is the honest metric.

---

## Before vs After

Same scene (e.g. case A, 100x100 dirty rect fully inside an opaque window):

    Before: blit_rect_db copies 40,000 bytes base->back, then window overwrites
    After : blit_rect_db skipped, window writes the same pixels
    Delta : 40,000 fewer bytes copied per such region (MEASURED on host)
    Result: rgb_diff = 0, no sentinel

All non-covered cases are byte-for-byte identical to before (base blit kept).

---

## Interaction With Phase 14

Uses the existing kwm_window_t.fully_opaque flag; no new opacity mechanism. The
kernel validation gate is unchanged and not weakened. fully_opaque == 1 becomes
the candidate for BOTH the Phase 14 content memcpy fast-path AND this base-blit
elision. fully_opaque == 0 (libgui/libui/desktop) is unaffected: normal scalar
composition and normal base blit.

---

## Interaction With Phase 15

The check runs per final coalesced dirty region, independently. Phase 15's
coalescing algorithm is not modified. Multiple regions are handled separately
(case H): covered regions skip, others keep the base blit. Regions are not
re-collapsed and no sub-splitting is introduced.

---

## Known Limitations

1. Trigger requires a fully_opaque window whose CONTENT fully contains the dirty
   rect. Currently only Rust/Slint windows qualify (Phase 14). Real workloads
   that move windows, draw on the desktop, or update libgui/libui windows do not
   benefit — and are unchanged.
2. Regions spanning a window edge keep the full base blit (no sub-rectangle
   splitting was added; the spec explicitly defers that).
3. On-screen validation of a live Rust window was not automatable (login
   password unknown); the skip path is validated by the host harness against the
   real flush plus QEMU boot/input regression.
4. The optimization is per-region and does not attempt occlusion culling or
   multi-window unions.

---

## Files Modified

- kernel/gfx/compositor.c — added rect_covers() and
  rect_covered_by_opaque_window(); compositor_flush() now guards blit_rect_db
  with the coverage check.

## Files Not Modified

kernel/display.c and include/display.h (Phase 15 coalescing untouched),
kernel/gfx/kwm.c, kernel/gfx/kwm_internal.h (Phase 14 opacity untouched),
kernel/syscall.c, GHAL, VirtIO-GPU, libgui, libui, Rust/Slint.

(Other files shown modified in git are from Phase 14/15 and earlier.)

---

## Phase 17 Recommendation

The remaining Phase 11 item is window-iteration control cost (all 16 slots x all
z-levels scanned per dirty region, compositor.c). It is bounded and secondary;
the higher-value follow-up is to SPLIT a dirty region at opaque-window edges so
the covered sub-rect also benefits (the Phase 16 known limitation #2). That
split must preserve the sum-of-contained-rects coverage invariant and stay
within the existing bounded region list. GPU remains deferred: the remaining
costs are CPU-side and small for the common workloads.

---

## Acceptance Mapping

- Production code modified: yes (kernel/gfx/compositor.c)
- Base blit skipped for fully-covered validated-opaque window: yes
- Partial coverage does NOT skip: yes (case C)
- Non-opaque windows unaffected: yes (case E)
- Z-order respected: yes (proof + case F)
- Window clipping respected: yes (case G)
- Decorations/shadows/cursor unchanged: yes (only the base copy is conditional)
- Multiple dirty regions independent: yes (case H)
- Phase 14 validation unchanged: yes
- Phase 15 coalescing unchanged: yes
- Pixel-identical to baseline, rgb_diff == 0: yes (all host cases)
- Kernel / Rust / ISO build: yes
- QEMU runtime test: yes (boot + keyboard + mouse, no panic)
- No fabricated performance numbers: yes (byte counts MEASURED; CPU speedup not claimed)
- Report created: yes
