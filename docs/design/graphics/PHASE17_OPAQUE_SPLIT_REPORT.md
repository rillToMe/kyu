# Phase 17 — Dirty Region Splitting at Opaque Window Edges

Date: 2026-09-14
Status: COMPLETE — OPAQUE SPLITTING VALIDATED

---

## Status

Production code changed in one kernel file (kernel/gfx/compositor.c). A dirty
rect only PARTLY covered by a validated fully-opaque window is no longer
base-blitted in full: only the uncovered remainder is copied, and the existing
composition still runs once over the whole rect. A host harness drives the REAL
compositor_flush() and compares it to a forced-base-blit baseline across
thirteen scenes: rgb_diff == 0 and no uninitialized pixel in every case, with
base-blit bytes measured. Kernel, Rust apps and the hybrid ISO build; QEMU boots
and responds to keyboard and mouse with no panic.

---

## Objective

Phase 16 could skip the base copy only when an opaque window covered the WHOLE
dirty rect. When an opaque window covered only part of it, the base copy still
ran for the entire rect. Phase 17 removes the base copy for the covered part
while keeping it for the uncovered part.

---

## Phase 16 Limitation

    dirty rectangle
    +-----------------------------+
    |        |                    |
    | OPAQUE |    uncovered       |
    | WINDOW |                    |
    +-----------------------------+

Phase 16: blit the WHOLE rect, then compose. The coverage overlap is wasted.
Phase 17: blit only the "uncovered" remainder, compose the whole rect once.

---

## Implementation

kernel/gfx/compositor.c:

- Removed the Phase 16 pair rect_covers() / rect_covered_by_opaque_window();
  their job is now the full-cover branch of the new helper.
- Added base_blit_for_region(back_db, screen_db, r):
    * scans the bounded window slots (MAX_WINDOWS = 16) for the largest
      intersection of r with a composited fully-opaque window's CONTENT rect;
    * full cover  -> no base copy at all (Phase 16 behaviour);
    * no cover / cover below KWM_SPLIT_MIN_PIXELS -> one whole-rect base copy;
    * partial cover -> base-blit only r minus the covered sub-rect, as up to
      four disjoint strips (top, bottom, left, right).
- compositor_flush(): the per-region loop now calls base_blit_for_region(r)
  followed by composite_windows_in_rect(r, pitch4) — unchanged composition.

No other files changed. No new syscalls, no userspace API, no allocation,
no changes to Phase 14 validation or Phase 15 coalescing.

---

## Rectangle Splitting Algorithm

Given dirty rect r and covered sub-rect best = r INTERSECT content(W), where
best is non-empty and strictly smaller than r, the uncovered remainder is
decomposed into at most four disjoint strips:

    top    { r.x, r.y, r.w, best.y - r.y }                      if best.y > r.y
    bottom { r.x, best.y+best.h, r.w, r.y+r.h - (best.y+best.h) } if ... < r.y+r.h
    left   { r.x, best.y, best.x - r.x, best.h }                if best.x > r.x
    right  { best.x+best.w, best.y, r.x+r.w - (best.x+best.w), best.h } if ... < r.x+r.w

top/bottom span the full width; left/right are limited to the covered vertical
band, so the four strips do not overlap and together equal r minus best.
All arithmetic is 64-bit; every strip is emitted only when its size is > 0.
These strips plus best partition r exactly.

---

## Coverage / Safety Invariant

    union(uncovered strips) UNION best  ==  r        (partition)

- Covered sub-rect best: not base-blitted; the opaque window writes every pixel
  of it during composition (validated opaque content, clipped to r).
- Uncovered strips: base-blitted.
- Composition then runs over the WHOLE r, so any pixel not base-blitted and not
  covered by the opaque window would remain uninitialized — the host harness
  fills the backbuffer with a sentinel and asserts zero sentinel pixels remain
  in every dirty region. All cases pass.

Over-invalidation is allowed; under-invalidation is not. The transform only
ever covers r, never less.

---

## Z-Order Handling

Z-order is unchanged and composition is unchanged. The helper does NOT require
the opaque window to be topmost, for the same reason as Phase 16: an opaque
write to every pixel of best is enough, and later windows only draw on top
(their transparent pixels leave the opaque layer showing). A window above the
opaque one with transparent holes is handled correctly (host case I). The
optimization only removes base_canvas -> backbuffer bytes; it never skips
window composition.

Defensive guard: the helper ignores any window with z_index > next_z_index
(i.e. one the compositor would not draw), so it never elides the base copy for
a window that is not actually composited.

---

## Phase 14 Interaction

Reuses kwm_window_t.fully_opaque unchanged. No new opacity mechanism. The
kernel validation gate is untouched and not weakened. Non-opaque windows never
cause a skip (host case G). Base-blit elision is now the combined Phase 16/17
full-cover and partial-cover paths.

---

## Phase 15 Interaction

The split runs at compositor CONSUMPTION time, after Phase 15 has produced the
final bounded dirty list. Phase 15's coalescing is not modified; the region
count policy and superset invariant are untouched. Each final region is split
independently (host case F).

---

## Host Correctness Tests

Harness (OS temp dir) compiles the REAL kernel/gfx/compositor.c and
kernel/display.c for the host. spinlock cli/sti would fault in ring 3, so the
lock helpers are macro-mocked to no-ops (nothing else altered). Optimized path
= real compositor_flush(); baseline = a reconstructed forced-base-blit loop.
Backbuffer is pre-filled with a sentinel (0x00DEAD00) so any un-written pixel is
detected. Base-blit bytes are measured by wrapping viewport_render at link time
(--wrap), so there is no instrumentation in production code.

MEASURED (host), W=400 H=300:

    Case  Scene                                  regions rgb_diff sentinel baseB  optB  calls red
    A     dirty fully inside opaque window        1       0        0       40000     0     0  100%
    B     opaque covers left half                 1       0        0       40000  20000    1   50%
    C     opaque covers center (4 strips)         1       0        0       40000  30000    4   25%
    D     opaque covers right half                1       0        0       40000  20000    1   50%
    E     no intersection                         1       0        0       25600  25600    1    0%
    F     two regions, independent                2       0        0       20800  14400    1   31%
    G     fully covering but NON-opaque window    1       0        0       14400  14400    1    0%
    H     opaque under a non-opaque window        1       0        0       57600     0     0  100%
    I     transparent pixels in upper window      1       0        0       57600     0     0  100%
    J     opaque window partially off-screen      1       0        0       25600     0     0  100%
    K     normal window with titlebar offset      1       0        0       10000     0     0  100%
    L     cover below KWM_SPLIT_MIN_PIXELS        1       0        0       40000  40000    1    0%
    M     opaque window larger than dirty rect    1       0        0        6400     0     0  100%

    rgb_diff == 0 and sentinel == 0 in every case.

Key results:
- B/C/D: partial covers split correctly; the uncovered strips are base-blitted,
  the covered sub-rect is not; pixel-identical to baseline.
- C shows the four-strip decomposition (4 blit calls, 25% fewer bytes).
- E/G/L: no safe cover -> full base copy, exactly as before.
- H/I: opaque window under another window (including one with transparent holes)
  is a valid elision source and stays pixel-identical.
- J/K: off-screen clipping and the 24px titlebar content offset are respected.
- M: window larger than the dirty rect -> full cover, no base copy.

---

## QEMU Tests

QEMU 10.2.1 (E:\Tools\msys2\mingw64\bin). Hybrid ISO booted with -display none,
serial to file, monitor on TCP.

- No panic / exception / fault.
- Login screen renders (1280x800).
- sendkey changed the framebuffer (CRC) — live incremental damage works.
- mouse_move changed the framebuffer (CRC) — cursor path works.
- No stale pixels, trails, black regions or corruption observed.

Limitation: launching a Rust/Slint window (the opaque producer) requires logging
into the image; the persisted password on disk.img is unknown, so automated
app-window testing was not performed. The split logic is validated by the host
harness against the real compositor_flush; QEMU confirms the integration is
regression-free.

---

## Performance Measurements

MEASURED (host harness):
- Base-blit bytes before vs after per scene (table above). Reduction ranges from
  0% (no safe cover) to 100% (full cover); partial covers land in between,
  exactly proportional to the covered fraction (B/D 50%, C 25%, F 31%).
- Blit call count rises only for split cases (C: 4 strips in one region).

INFERRED (source-derived) for real workloads:
- A Rust/Slint window is the only current fully_opaque producer. Dirty regions
  that straddle a Rust window's edge (e.g. window move: old+new frame bounds, or
  a region spanning the window border and the desktop) now elide the base copy
  for the portion inside the window content.
- Regions fully over the desktop/TTY or over libgui/libui windows have no opaque
  cover -> unchanged.

NOT MEASURED / NOT CLAIMED:
- No CPU-time speedup figure and no per-frame kernel counters. The honest metric
  is base-blit byte reduction, measured on the host against the real flush.

---

## Before vs After

Same scene (case B: 100x100 dirty, opaque window over the left 50%):

    Before (Phase 16): blit_rect_db copies the full 100x100 (40,000 B),
                       then compose; the left half is overwritten.
    After  (Phase 17): base-blit only the right strip 50x100 (20,000 B),
                       then compose the whole rect; the left half is written
                       by the opaque window.
    Delta : 20,000 fewer base bytes per such region (MEASURED), rgb_diff = 0.

For a fully-covered region the result is identical to Phase 16 (zero bytes).

---

## Performance Overhead

- The helper replaces the Phase 16 scan (same O(MAX_WINDOWS) cost) and adds at
  most four rectangle computations plus up to three extra blit calls when a
  partial cover is split.
- Composition is NOT multiplied: composite_windows_in_rect(r) is still called
  exactly once per region on the full rect (not per strip). Only the base blit
  is decomposed.
- Guard KWM_SPLIT_MIN_PIXELS = 1024: a cover smaller than ~32x32 falls back to a
  single whole-rect base blit, because below that the extra call overhead
  outweighs the bytes saved. Case L verifies the guard (0% reduction, one call).
- Splitting only triggers when an opaque window partially covers the region;
  the common no-opaque-window update takes the same single-blit path as before.

---

## Known Limitations

1. One opaque window per region: only the largest intersection is used. If
   several opaque windows tile a region, only one cover benefits this frame; the
   rest stays base-blitted. Bounded by design (no general occlusion solver).
2. Regions with no opaque cover are unchanged (desktop/TTY/libgui/libui).
3. Real Rust-window workloads were not exercised in QEMU (unknown login
   password); validation is the host harness on the real flush plus QEMU
   boot/input regression.
4. The 1024-pixel split threshold is a heuristic; it is a single tunable
   constant, not measured against a full workload suite.

---

## Files Modified

- kernel/gfx/compositor.c — removed rect_covers / rect_covered_by_opaque_window;
  added KWM_SPLIT_MIN_PIXELS and base_blit_for_region(); compositor_flush() now
  calls base_blit_for_region() per region.

## Files Not Modified

kernel/display.c and include/display.h (Phase 15 coalescing), kernel/gfx/kwm.c,
kernel/gfx/kwm_internal.h (Phase 14 opacity), kernel/syscall.c, GHAL,
VirtIO-GPU, libgui, libui, Rust/Slint. (Other modified files in git predate
Phase 17.)

---

## Phase 18 Recommendation

The remaining Phase 11 item is window-iteration control cost (all 16 slots x all
z-levels scanned per region), which is bounded and secondary. Higher value:
allow the split to consume MORE THAN ONE opaque window per region (subtract each
opaque cover in turn, still bounded), so scenes with multiple tiled opaque
windows elide more of the base copy. That is a small extension of
base_blit_for_region with the same coverage invariant. GPU remains deferred: the
remaining costs are CPU-side and small for the common workloads.

---

## Acceptance Mapping

- Production code modified: yes (kernel/gfx/compositor.c)
- Partial dirty rects split at opaque-window edges: yes (B/C/D, F)
- Fully covered subregions skip the base blit: yes
- Uncovered subregions still base-blitted: yes
- No dirty pixels lost: yes (partition invariant + sentinel == 0)
- Coverage invariant passes: yes (sentinel check on every case)
- rgb_diff == 0 vs baseline: yes (all 13 cases)
- Non-opaque windows unaffected: yes (case G)
- Phase 14 validation unchanged: yes
- Phase 15 coalescing unchanged: yes
- Z-order preserved: yes (no reorder; proof + case H)
- Transparent windows above opaque remain correct: yes (case I)
- Cursor / decorations / shadows unchanged: yes (only base blit is decomposed)
- No new syscall / userspace API: yes
- No dynamic allocation: yes (fixed local rects only)
- Kernel stack safe: yes (no arrays; a few scalar locals)
- Kernel / Rust / ISO build: yes
- QEMU runtime regression: yes (boot + keyboard + mouse, no panic)
- Measurements honest and labeled: yes (bytes MEASURED; CPU speedup not claimed)
- Report created: yes
