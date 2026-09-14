# Phase 15 — Dirty Region Coalescing

Date: 2026-09-14
Status: COMPLETE — COALESCING IMPLEMENTED

---

## Status

Production code changed in two kernel files. Bounded rectangle coalescing
replaces the collapse-to-one-bbox behavior. A host harness validates the
superset invariant on the real kernel/display.c across seven synthetic cases
(including 300-mark pressure and a dense TTY grid). Kernel, Rust apps and the
hybrid ISO all build; QEMU boots, and typed input plus mouse movement both
redraw correctly with no panic. No dirty pixel is ever lost.

---

## Problem

When more than MAX_DIRTY_REGIONS (64) rectangles accumulated between flushes,
kernel/display.c replaced the entire dirty list with ONE bounding box. Sparse,
independent updates (scattered glyph cells, widget hovers, a few windows) then
made the compositor base-blit, composite, upload and present the whole spanned
area. Phase 11 ranked this the #1 work amplifier. Worse, the list filled with
duplicate/overlapping rects because marks were appended blindly.

---

## Existing Behavior (before)

    dirty_region_mark(list, r):
        if collapsed: regions[0] = union(regions[0], r); return
        if count >= MAX:  regions[0] = union(all regions + r);
                          count = 1; collapsed = 1; return
        regions[count++] = r

No overlap detection, no containment handling, no merge heuristic. The first
mark past 64 collapsed everything.

---

## Implementation

Files changed (only these two; everything else is Phase 14 or earlier):

- include/display.h — updated the DirtyRegionList contract comment. The
  `collapsed` field is retained for struct layout compatibility but is no
  longer used by the algorithm.
- kernel/display.c — new helpers plus a rewritten dirty_region_mark:
  - rect_area_u64(Rect)          : area as uint64.
  - merge_inflation(Rect,Rect)   : SIGNED bounding-box cost of a merge.
  - rect_contains(outer,inner)   : 64-bit-safe containment.
  - dirty_drop_contained(...)    : compact out regions covered by a grown one.
  - dirty_region_mark(...)       : coalescing insert (below).

dirty_region_clear() is unchanged apart from keeping the collapsed reset.

---

## Coalescing Algorithm

dirty_region_mark(list, r):

1. Already covered — if any existing region contains r, return (no new region).
2. Merge into the overlapping region with the lowest merge cost, but only when
   that cost is <= 0 (the bounding box is no larger than processing the two
   separately). A non-positive cost also removes double-processing of the
   overlap. Lightly-overlapping rects that would inflate stay separate.
3. Otherwise append while there is room. Regions r fully covers are dropped
   (r supersedes them).
4. If the list is full, do NOT collapse to one bbox. Choose the least wasteful
   bounded action: grow one region to include r, or merge the cheapest existing
   pair and keep r separate. Count stays <= MAX.

Every branch only ever unions or appends r (and drops only regions fully
contained in a kept region), so coverage never shrinks.

---

## Merge Cost Function

    merge_inflation(a,b) = bbox_area(union(a,b)) - area(a) - area(b)

SIGNED and 64-bit. Rationale:

- Two overlapping rects: the bounding box can be SMALLER than the sum, so the
  cost is negative and the merge is a strict win (fewer pixels, no double
  processing of the shared overlap).
- Two separated rects: the cost is positive and equals the wasted gap.
- Unsigned would wrap negatives into huge values and invert every comparison;
  the function must be signed.

Step 2 only merges when the cost is <= 0 (never creates work). Under MAX
pressure (step 4) the code is forced to merge something, so it picks the
minimum cost option (least wasted area), preferring non-positive merges first,
with deterministic index-order tie-breaking (no randomness).

---

## MAX Region Handling

The 65th and later marks never allocate a new slot and never collapse the list.
They merge the cheapest pair (or absorb r into the cheapest region) so the count
stays at MAX. O(N^2) pair search is acceptable: N = 64, and this path runs only
when the list is already full. Normal marks cost O(N) (containment/overlap scan)
which is small next to the draw that produced them.

---

## Correctness Invariant

    union(coalesced regions)  is a SUPERSET of  union(requested rects)

Over-invalidation is allowed (redrawing extra pixels is safe); under
invalidation is forbidden (a missed pixel leaves stale content on screen).
Every transform grows coverage via rect_union or appends the new rect, so the
invariant holds by construction. The host harness checks it explicitly per case.

---

## Host Tests

Harness (OS temp dir) compiles the REAL kernel/display.c for the host with
kmalloc/kfree stubs and an exact coverage bitmap (1024x1024, one byte per pixel
— true union, NOT a sum of areas). It also re-implements the OLD collapse
algorithm for a before/after comparison. Every case asserts
original_mask is a subset of coalesced_mask.

MEASURED results (coalesced = total pixels the compositor would process;
old = same metric under the previous collapse):

    A) 120 scattered 6x6 marks      regions=64  orig_union=4304    coalesced=46749   old=1022096
    B) duplicate rect x100          regions=1   orig_union=400     coalesced=400     old=400
    C) nested 60x60 then 10x10      regions=1   orig_union=3600    coalesced=3600    old=3600
    D) 65 disjoint columns          regions=64  orig_union=26000   coalesced=26200   old=38800
    E) full-screen then 20 small    regions=1   orig_union=1048576 coalesced=1048576 old=1048576
    F) adjacency/edge/negative      regions=4   orig_union=36      coalesced=36      old=36
    G) 300 random small marks       regions=64  orig_union=4433    coalesced=24324   old=63252
    H) dense TTY 80x25 glyph grid   regions=64  orig_union=256000  coalesced=256000  old=256000

    superset invariant: OK in all cases (no dirty pixel lost)

Highlights:
- A: sparse scatter across the screen — old collapsed to the full-screen bbox
  (1022096 px); new keeps 64 local regions (46749 px) = roughly 22x less.
- D: 65 disjoint columns — old collapsed to 38800 (one bbox); new 26200 = 1.48x less.
- G: sustained 300-mark pressure — old 63252 (full bbox); new 24324 = 2.6x less.
- B, C, E, H: identical to old (dedup/nested/full-screen/dense already optimal);
  H (dense TTY) legitimately covers the full grid in both — no regression.
- F: adjacency/edge/negative/zero-size handled; coverage unchanged.

---

## QEMU Tests

QEMU 10.2.1 (E:\Tools\msys2\mingw64\bin). Booted the hybrid ISO with
-display none, serial to file, monitor on TCP. No panic / exception / fault.

Functional (MEASURED), framebuffer captured via `screendump` at 1280x800:

- Login screen renders (1,023,939 non-black pixels).
- `sendkey r o o t ret` changed the framebuffer (CRC changed) — incremental
  damage redraw works.
- `mouse_move` changed the framebuffer (CRC changed) — cursor dirty rects work.
- No stale pixels, trails or corruption observed across the captures.

Screenshots were deleted after the run; no artifacts left in the repo.

---

## Real Workload Measurements

- Interactive workloads (hover, click, cursor, clock, taskbar) typically emit
  far fewer than 64 marks per frame, so they never reached the old collapse
  path. Their behavior is unchanged except that duplicate/contained marks now
  collapse to fewer regions (strictly less compositor work).
- The collapse path was only reached by bursts: a TTY full repaint, a widget
  tree redraw, or a Slint animation. A dense TTY repaint legitimately covers the
  whole area in both old and new (case H) — no improvement is expected there.
- The measurable win is SPARSE bursts (case A/D/G), exactly the scenario Phase
  11 flagged.
- Per-frame region counts under real QEMU workloads were NOT instrumented: no
  temporary counters were added to the kernel (instrumentation must be removed;
  none is needed to prove correctness). Real-workload region-count numbers are
  therefore not reported — no fabricated figures.

Note: the synthetic harness is SYNTHETIC input over the real algorithm. QEMU
provides REAL functional confirmation, not per-frame counters.

---

## Before vs After

    Scenario                       Before (old)      After (new)      Delta
    sparse scatter (A)             1022096           46749            ~22x less
    65 disjoint columns (D)        38800             26200            1.48x less
    300-mark pressure (G)          63252             24324            2.6x less
    dense TTY grid (H)             256000            256000           1.00x (same)
    duplicate / nested (B,C)       optimal           optimal          same
    full-screen (E)                full              full             same

    All figures MEASURED on the host harness (exact coverage bitmap).
    QEMU: functional pass (typing + mouse redraw, no panic); no per-frame counters.

---

## Performance Analysis

- Region count is hard-bounded at MAX_DIRTY_REGIONS = 64 (no 65th slot, no
  collapse). Memory unchanged (fixed array).
- Producer cost: O(1) best case (contained/duplicate), O(N) normal (overlap
  scan, N <= 64), O(N^2) only when the list is full. Bounded and small next to
  the drawing that emitted the marks.
- Compositor cost: bounded by sum of region areas. The old worst case (one
  full-screen bbox) is eliminated for sparse bursts.
- The heuristic can be slightly worse than "keep every rect separate" only in
  that it still bounds the count; bounded count is required for the fixed
  array, and the merged area is minimized rather than maximized.

Classification: host figures MEASURED; QEMU functional MEASURED; compositor
CPU-time speedup NOT measured (no cycle counters) — not claimed.

---

## Regressions

- Kernel builds clean (no new warnings/errors). Rust apps build. ISO builds.
- QEMU boots to login, no panic; typing and mouse redraw correctly.
- Phase 14 opaque fast-path untouched: compositor.c, kwm.c, kwm_internal.h,
  syscall.c and the Rust crates were not modified this phase. The coalescer
  changes only which rects reach the compositor, not how a rect is composited.
- Dirty semantics preserved: superset invariant verified per case, including
  full-screen, edges/negative coords and zero-size marks.
- Cursor-rect folding in compositor_flush (which calls dirty_region_mark on the
  local snapshot) uses the same coalescer and remains correct.

---

## Files Modified

- include/display.h — DirtyRegionList contract comment (collapsed now unused).
- kernel/display.c — merge_inflation, rect_area_u64, rect_contains,
  dirty_drop_contained, rewritten dirty_region_mark.

## Files Not Modified

kernel/gfx/compositor.c, kernel/gfx/kwm.c, kernel/gfx/kwm_internal.h,
kernel/syscall.c, GHAL, VirtIO-GPU, libgui, libui, Rust/Slint sources,
scheduler, heap. (Those show as modified in git from Phase 14 and earlier only.)

## Known Limitations

1. Bbox merging is area-optimal per pair but greedy; a chain of merges under
   sustained pressure can still inflate (case G: 5.49x over the exact union).
   Old behavior was far worse (14.3x there, and full-screen collapse elsewhere).
   A future phase could add a small relative slack or split large inflated
   regions, but that risks under-invalidation and more complexity.
2. The `collapsed` field is now vestigial (kept only for struct compatibility).
3. Dense full-screen repaints are not improved (they are already optimal).

## Phase 16 Recommendation

The measured remaining amplifier is the BASE BLIT of fully-covered opaque
regions (Phase 11 duplicate-work #2): for a dirty rect fully covered by a
validated-opaque window (Phase 14), the base_canvas blit is copied and then
immediately overwritten. Reuse the Phase 14 `fully_opaque` metadata to skip the
base blit where an opaque window covers the whole dirty rect. That is the
highest-value next step and stays localized to the compositor. GPU remains
deferred.
