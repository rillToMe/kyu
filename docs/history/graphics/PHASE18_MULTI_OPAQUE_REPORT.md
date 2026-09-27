# Phase 18 — Multi-Opaque Window Coverage

Date: 2026-09-14
Status: COMPLETE — MULTI-OPAQUE COVERAGE VALIDATED

---

## Status

Production code changed in one kernel file (kernel/gfx/compositor.c). The
compositor now subtracts the content of MORE THAN ONE validated fully-opaque
window from a dirty region before deciding what needs a base copy. Composition
is unchanged and still runs exactly once on the original region. A host harness
drives the real compositor_flush() against a forced-base-blit baseline over 14
named cases plus 400 randomized scenes: rgb_diff == 0 and zero sentinel pixels
in every case, with base-blit bytes measured. Kernel, Rust apps and the hybrid
ISO build; QEMU boots and responds to keyboard and mouse with no panic.

---

## Objective

Eliminate more base_canvas -> back_buffer traffic by letting several opaque
windows each remove their covered part from the base-copy work, while keeping
the operation strictly bounded and leaving composite_windows_in_rect() called
exactly once per dirty region.

---

## Phase 17 Limitation

Phase 17 used only the SINGLE largest opaque intersection per dirty region.
With two opaque windows covering different parts of a region, only one
contributed and the rest was still base-blitted.

---

## Implementation

kernel/gfx/compositor.c, one helper (base_blit_for_region) rewritten and two
small helpers added:

- rect_covers(outer, inner): 64-bit containment test.
- split_push(out, &m, &overflow, p): append a non-empty strip; sets overflow.
- window_content_rect(w): content geometry, mirroring composite_windows_in_rect
  (y + titlebar; titlebar = 0 for the frameless desktop).
- base_blit_for_region(back_db, screen_db, r):
    remain = { r }
    repeat up to KWM_MAX_OPAQUE_SPLITS times:
        pick the opaque window whose content covers the most of `remain`
        (already-subtracted windows score 0, so they are not re-picked)
        stop if none intersects, or the cover is below KWM_SPLIT_MIN_PIXELS
          and does not fully remove a remaining rect
        subtract the window content from every remaining rect into `out`
        if `out` would exceed KWM_MAX_SPLIT_RECTS: abort this step, keep
          `remain` untouched, and stop
        remain = out
    base-blit every rect left in `remain`

compositor_flush() is unchanged structurally: it still calls
base_blit_for_region(r) then composite_windows_in_rect(r, pitch4) ONCE per
dirty region.

No other files changed. No new syscalls, no userspace API, no allocation.

---

## Bounded Window Selection

Greedy and deterministic: at each step the window with the largest total
intersection area against the current remainder list wins. Ties keep the first
window found (index order). Complexity is O(KWM_MAX_OPAQUE_SPLITS * MAX_WINDOWS
* KWM_MAX_SPLIT_RECTS) = at most 4 * 16 * 12 intersection tests, all integer,
no allocation, no spatial index. A window already subtracted has zero
intersection with the remainder, so it is naturally excluded.

---

## Rectangle Subtraction

For each remaining rect R and chosen content C, with I = R INTERSECT C:

    top    { R.x,          R.y,        R.w, I.y - R.y }
    bottom { R.x, I.y+I.h,             R.w, R.y+R.h - (I.y+I.h) }
    left   { R.x,          I.y,        I.x - R.x,        I.h }
    right  { I.x+I.w,      I.y,        R.x+R.w - (I.x+I.w), I.h }

Each strip is emitted only when its size is > 0, so maximum four disjoint
strips that exactly equal R minus I. If R did not intersect C, R is carried
through unchanged.

---

## Region Bound

KWM_MAX_SPLIT_RECTS = 12 (stack arrays, no allocation). If a subtraction would
produce more than 12 remainder rects, that subtraction is abandoned, the
previous `remain` list is kept, and the loop stops: the still-uncovered area is
base-blitted. KWM_MAX_OPAQUE_SPLITS = 4 bounds how many opaque windows are
consumed. Both bounds only ever cause MORE base blitting, never less — so
hitting a bound is always safe.

---

## Correctness Invariant

    original dirty rect r
        =  union(subtracted opaque covers)
           UNION
           union(final remainder rects)

Every subtracted cover lies entirely inside r and inside a validated opaque
window CONTENT rect, so composition writes every pixel of it. The final
remainders are base-blitted. Therefore every pixel of r is either base-blitted
or written by an opaque window; nothing is left uninitialized. The harness
verifies this directly with a sentinel fill: zero sentinel pixels remain.
Over-invalidation is allowed (bounding/aborting), under-invalidation is not.

---

## Host Harness

Harness (OS temp dir) compiles the REAL kernel/gfx/compositor.c and
kernel/display.c for the host. spinlock cli/sti would fault in ring 3, so the
lock helpers are macro-mocked to no-ops (nothing else altered). Optimized path
= real compositor_flush(); baseline = a reconstructed forced-base-blit loop.
Backbuffer is pre-filled with a sentinel (0x00DEAD00); base-blit bytes are
measured by wrapping viewport_render at link time (--wrap), so production code
carries no instrumentation.

MEASURED (host), W=400 H=300:

    Case  Scene                                  regions rgb_diff sentinel baseB   optB  calls red
    A     one opaque window, full cover           1       0        0       40000      0    0  100%
    B     two disjoint opaque (middle uncovered)  1       0        0      240000  80000    1   67%
    C     two adjacent opaque                     1       0        0       80000      0    0  100%
    D     two overlapping opaque                  1       0        0      193600  92800    7   52%
    E     three opaque tiling                     1       0        0      120000      0    0  100%
    F     opaque + non-opaque                     1       0        0      120000  80000    1   33%
    G     opaque below transparent window         1       0        0       40000      0    0  100%
    H     opaque below opaque upper               1       0        0       40000      0    0  100%
    I     cover below threshold                   1       0        0       40000  40000    1    0%
    J     covered after 2nd window                1       0        0       80000      0    0  100%
    K     more opaque windows than bound (5)      1       0        0      100000  20000    1   80%
    L     off-screen opaque window                1       0        0       25600   6000    2   77%
    M     titlebar/frame offset                   1       0        0       14400      0    0  100%
    N     multiple dirty regions                  2       0        0       10000      0    0  100%

    rgb_diff == 0 and sentinel == 0 in every case.

Interpretation:
- B/D/E/J: multiple opaque windows each contribute; coverage compounds. B saves
  67% using BOTH windows (Phase 17 would use one). J reaches zero base bytes
  once the second window closes the gap.
- C: two adjacent windows tile the region -> zero base bytes.
- D: overlapping windows -> no double counting; 52% saved.
- F: a non-opaque window is never an opacity source (only the 1/3 opaque part is
  eliminated).
- G/H: opaque coverage remains valid under transparent and under opaque upper
  windows; rgb identical to baseline.
- I: the small-cover guard prevents splitting.
- K: 5 opaque windows > KWM_MAX_OPAQUE_SPLITS (4): four are consumed and the
  fifth strip is base-blitted (80% not 100%) — the bound falls back safely.
- L/M: off-screen clipping and the 24px titlebar content offset respected.
- N: each dirty region handled independently.

Randomized geometry fuzz (deterministic seed, 400 scenes; 0-4 windows, random
positions/sizes, opaque or non-opaque, random transparent holes, 1-3 dirty
regions):

    failures = 0   sentinel_total = 0
    aggregate base_bytes = 16,535,664   opt_bytes = 15,984,900   reduction 3.3%

The low aggregate reduction is expected: random windows are mostly non-opaque
or do not tile, so most scenes have little opaque coverage. The point of the
fuzz is correctness (zero failures, zero sentinel), not throughput.

---

## QEMU Validation

QEMU 10.2.1 (E:\Tools\msys2\mingw64\bin). Hybrid ISO booted with -display none,
serial to file, monitor on TCP.

- No panic / exception / fault.
- Login screen renders (1280x800).
- sendkey changed the framebuffer (CRC) — live incremental damage works.
- mouse_move changed the framebuffer (CRC) — cursor path works.
- No stale pixels, black regions, trails or corruption observed.

Limitation: a Rust/Slint window (the only current opaque producer) cannot be
launched automatically (the persisted login password on disk.img is unknown),
so multi-window on-screen testing was not performed. The multi-opaque logic is
validated by the host harness against the real compositor_flush; QEMU confirms
the integration is regression-free.

---

## Performance Measurements

MEASURED (host): base-blit byte reduction per scene (table above). Multi-window
scenes gain over Phase 17: B 67% (was up to 33% with one window), C/J/E 100%,
K 80% (bounded), D 52%.

INFERRED (source-derived) for real workloads:
- Only Rust/Slint windows are fully_opaque today. A dirty region straddling two
  Rust windows, or a window move whose old+new bounds span several opaque
  windows, now elides the base copy for each covered part.
- Regions over the desktop/TTY/libgui/libui have no opaque cover and take the
  cheap no-cover path (one scan, one base blit) — unchanged.
- Whether real scenes frequently put MULTIPLE opaque windows over one region is
  not measured; in current kyuzen layouts this is uncommon, so the incremental
  benefit over Phase 17 is expected to be modest in day-to-day use. The
  implementation is correct and bounded regardless.

NOT MEASURED / NOT CLAIMED: no CPU-time speedup figure; no per-frame kernel
counters. The honest metric is base-blit byte reduction, measured on the host
against the real flush.

---

## Before vs After

Scene (case B: 300x200 region, two disjoint opaque windows covering the left and
right thirds):

    Phase 17: one window eliminates its third; base copy = 2/3 of the region.
    Phase 18: both windows eliminate their thirds; base copy = 1/3 (the middle).
    Delta   : 80,000 fewer base bytes (MEASURED), rgb_diff = 0.

Case J: Phase 17 could not reach zero with one window; Phase 18 reaches zero
base bytes once two windows together cover the region.

---

## Phase 14 Interaction

Reuses kwm_window_t.fully_opaque unchanged; no new opacity mechanism; the
kernel validation gate is untouched. Non-opaque windows are never selected as
coverage (case F). The opaque memcpy compositor fast-path in
composite_windows_in_rect is untouched.

---

## Phase 15 Interaction

The split runs at compositor CONSUMPTION time, after Phase 15 has produced the
final bounded dirty list. Phase 15 coalescing and its superset invariant are
not modified. Each final region is processed independently (case N).

---

## Phase 16 Interaction

Phase 16 (full rect covered by one opaque window -> no base copy) is the n==0
exit of the Phase 18 loop; case A still reports 100% reduction and zero blit
calls.

---

## Phase 17 Interaction

Phase 17 (largest single cover split) is the first iteration of the Phase 18
loop; the Phase 17 threshold KWM_SPLIT_MIN_PIXELS is preserved (case I) and the
four-strip subtraction is the same code, now applied repeatedly.

---

## Known Limitations

1. Only Rust/Slint windows are fully_opaque today, and current layouts rarely
   stack several opaque windows over one region, so the incremental benefit
   over Phase 17 is expected to be modest in practice (documented honestly).
2. Fragment bounds: with unlucky geometry a second subtraction can exceed
   KWM_MAX_SPLIT_RECTS and abort, leaving more base blitting (safe, suboptimal).
   Case K demonstrates the window-count bound.
3. Up to 4 opaque windows per region are consumed; beyond that, the remainder is
   base-blitted. This is deliberate (no general occlusion solver).
4. Live Rust-window testing in QEMU was not automatable (unknown login
   password); validation is the host harness on the real flush plus QEMU
   boot/input regression.

---

## Files Modified

- kernel/gfx/compositor.c — rewrote base_blit_for_region() for bounded
  multi-window subtraction; added rect_covers(), split_push(),
  window_content_rect(); added KWM_MAX_OPAQUE_SPLITS and KWM_MAX_SPLIT_RECTS
  (KWM_SPLIT_MIN_PIXELS preserved).

## Files Not Modified

kernel/display.c and include/display.h (Phase 15), kernel/gfx/kwm.c,
kernel/gfx/kwm_internal.h (Phase 14), kernel/syscall.c, GHAL, VirtIO-GPU,
libgui, libui, Rust/Slint, dirty-region producers. (Other modified files in git
predate Phase 18.)

---

## Phase 19 Recommendation

The base-blit path is now well optimized for opaque coverage. The remaining
Phase 11 item is window-iteration control cost (all 16 slots x all z-levels
scanned per region), which is bounded and secondary. A higher-value follow-up is
outside the compositor: give more window types a real opacity guarantee (the
Phase 14 kernel validation already exists) so libgui/libui windows can also
participate — that would widen the benefit of Phases 16-18. GPU remains
deferred: remaining costs are CPU-side and small for the common workloads.

---

## Acceptance Mapping

- Production code modified: yes (kernel/gfx/compositor.c)
- More than one opaque window contributes per region: yes (B/C/D/E/J/K)
- Strictly bounded: yes (max 4 windows, max 12 rects)
- No dynamic allocation: yes (fixed stack arrays)
- Subtraction preserves coverage: yes (partition + sentinel == 0)
- No dirty pixels lost: yes
- Sentinel test zero unexpected pixels: yes (all cases + 400 fuzz)
- rgb_diff == 0 vs baseline: yes
- Full cover still skips all base copy: yes (A/J)
- Partial cover saves additional bytes: yes (B 67% vs one window)
- Small-cover threshold effective: yes (I)
- Exceeding the bound safely falls back: yes (K)
- Non-opaque windows never opacity sources: yes (F)
- Transparent upper windows correct: yes (G)
- Z-order unchanged: yes (no reorder; only base blit decomposed)
- composite_windows_in_rect called once per original region: yes
- Phase 14/15/16/17 behaviour intact: yes
- Kernel / Rust / ISO build: yes
- QEMU regression: yes (boot + keyboard + mouse, no panic)
- Measurements honest: yes (bytes MEASURED; CPU speedup not claimed)
- Report created: yes
