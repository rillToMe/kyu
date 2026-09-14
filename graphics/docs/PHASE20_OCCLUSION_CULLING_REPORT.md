# Phase 20 — Opaque Occlusion Culling

Date: 2026-09-14
Status: COMPLETE — OCCLUSION CULLING VALIDATED

---

## Objective

Reduce compositor CPU work by not composing a window's content in pixels that a
VALIDATED fully-opaque window drawn in FRONT of it is guaranteed to overwrite.
Output must stay pixel-identical to the previous compositor.

---

## Existing Composition Pipeline

composite_windows_in_rect(r) walks z ascending, then window slots, and for each
window intersecting r draws: shadow frame (blend), content canvas (Phase 14
memcpy fast-path if fully_opaque, else scalar alpha test), and the WM titlebar.
Only the CONTENT canvas carries the kernel-validated fully_opaque guarantee;
titlebar/shadow are partial-alpha draws and are never treated as opaque.

Phase 18's base_blit_for_region() runs before this and is unchanged.

---

## Algorithm

Per dirty rect r, before the z-loop, collect the opaque occluders: every active,
composited (z_index <= next_z_index), fully_opaque window whose CONTENT rect
intersects r.

For each window W at (z, slot) being composited, its content is drawn only over:

    visible = clip  MINUS  union( content rect of every opaque window V
                                  with (V.z, V.slot) in front of (W.z, W.slot) )

"in front" is the exact draw order: V.z > W.z, or V.z == W.z and V.slot > W.slot.
The subtraction is rectangle-based (up to four disjoint strips per subtraction)
into a bounded stack list. If the whole clip is occluded, no content is drawn.

Only the content draw is culled. Shadows/titlebars are still drawn (the front
opaque content overwrites them later anyway), so their handling is unchanged.

---

## Safety Model

Only kernel-validated `fully_opaque` windows are occluders. A fully-opaque
window writes EVERY pixel of its content rect when composited (Phase 14), so
anything drawn underneath within that rect is dead. Non-opaque windows (normal,
transparent, or a rejected declaration) are never occluders: a front window with
alpha==0 pixels must let the window behind it show, so it must not cull.

The reasoning is chain-safe: for any pixel p covered by some front opaque
window, the FRONTMOST opaque window covering p is not culled at p (no opaque
window is in front of it there), so it writes p. Therefore culling W at p never
leaves p unwritten. Windows above a culled region still compose normally, so
transparent windows on top of opaque ones keep working (host case 13/14/15).

---

## Bounded Fallback

KWM_MAX_OCCLUSION_RECTS = 8 bounds the visible-area list (two 8-entry stack
arrays, no allocation). If a subtraction would exceed the bound, the culling for
that window is abandoned and the full clip is drawn — strictly more work, same
output. No rectangle is ever dropped, so coverage is never lost.

---

## Host Correctness Tests

Harness compiles the REAL kernel/gfx/compositor.c (locks macro-mocked; spinlock
cli/sti would fault in ring 3). Baseline = the SAME scene with every window's
fully_opaque forced to 0 (no culling, no opaque fast-path); optimized = real
flags. Both call the real compositor_flush(). Exact RGB comparison plus a
sentinel fill (0x00DEAD00) that catches any missed pixel; an oob column checks no
write lands outside the dirty regions.

Case | RGB diff | Sentinel | OOB
1 no occluder                    | 0 | 0 | 0
2 full occlusion                 | 0 | 0 | 0
3 left-half occlusion            | 0 | 0 | 0
4 center occlusion               | 0 | 0 | 0
5 top strip                      | 0 | 0 | 0
6 bottom strip                   | 0 | 0 | 0
7 corner occlusion               | 0 | 0 | 0
8 two disjoint occluders         | 0 | 0 | 0
9 overlapping occluders          | 0 | 0 | 0
10 occluder bigger than dirty    | 0 | 0 | 0
11 tiny intersection             | 0 | 0 | 0
12 off-screen occluder           | 0 | 0 | 0
13 transparent front (no occlude)| 0 | 0 | 0
14 multi-layer A opaque/B transp/C opaque | 0 | 0 | 0
15 mixed alpha front (0 and 0x80)| 0 | 0 | 0
16 rejected declaration          | 0 | 0 | 0
17 occluder over normal window   | 0 | 0 | 0
18 decorated (titlebar) front    | 0 | 0 | 0

All 18 cases: rgb_diff == 0, sentinel == 0, oob == 0.

Regression harnesses re-run against the changed compositor:
- Phase 18 (opaque base-blit coverage): 14 cases + 400-scene fuzz, 0 failures.
- Phase 19 (C/C++ opaque + kernel validation): all cases pass.

---

## Fuzz Results

    Scenes:            1500 (deterministic, seed 0x1234ABCD)
    Failures:          0
    Sentinel failures: 0
    Coverage:          random screen positions/sizes, z-order, transparent holes,
                       fully_opaque true/false, off-screen, overlapping, 1-3 dirty
                       regions, 1-5 windows per scene.

Each scene compared optimized vs baseline with exact RGB; zero mismatches.

---

## Performance

Measured with a TEMPORARY host-only pixel counter in mix_content (compiled only
in the test build via -DPHASE20_COUNT_PIXELS, removed from production after
measuring; the kernel build never defined it). Metric = content pixels passed to
composition (baseline vs optimized) for the same scene.

Scene | Before px | After px | Reduction
1 no occluder                       |  32400 |  32400 |   0%
2 full occlusion                    |  28800 |  14400 |  50%
3 left-half occlusion               |  72000 |  48000 |  33%
4 center occlusion                  |  71600 |  66000 |   8%
5 top strip                         |  81000 |  66000 |  19%
6 bottom strip                      |  81000 |  66000 |  19%
7 corner occlusion                  |  73200 |  66000 |  10%
8 two disjoint occluders            |  94400 |  81600 |  14%
9 overlapping occluders             | 110400 |  81600 |  26%
10 occluder bigger than dirty       |   6000 |   3000 |  50%
11 tiny intersection                |  81700 |  81600 |   0%
12 off-screen occluder              |  69500 |  66000 |   5%
13 transparent front (no occlude)   |  28800 |  28800 |   0%
14 multi-layer A/B/C                | 124600 | 102600 |  18%
15 mixed alpha front                | 145600 | 145600 |   0%
16 rejected declaration             |  28800 |  28800 |   0%
17 occluder over normal window      |  68960 |  50960 |  26%
18 decorated front                  | 103360 |  81600 |  21%
fuzz aggregate (1500 scenes)        | 3159590 | 3089174 | 2.2%

Notes:
- Cases 2/10 hit 50%: the occluder's own content must still be composed, so only
  the window(s) behind it are eliminated (their content was the other half).
- Cases 13/15/16 are 0%: a non-opaque window is not an occluder — exactly right.
- Fuzz aggregate is low because random scenes rarely stack opaque windows over
  each other; the win is workload-dependent.
- No FPS or CPU-time number is claimed (not measured).

---

## QEMU Validation

QEMU 10.2.1 (E:\Tools\msys2\mingw64\bin). Hybrid ISO booted with -display none,
serial to file, monitor on TCP:
- no panic / exception / fault;
- login screen renders (1280x800);
- sendkey changed the framebuffer (CRC) — keyboard path works;
- mouse_move changed the framebuffer (CRC) — cursor path works.

Limitation: the persisted disk.img has an unknown login password, so running
multiple overlapping windows on-screen was not automatable. Correctness evidence
is the host exact-pixel harness + 1500-scene fuzz against the real compositor;
QEMU confirms the image still builds, boots and renders without regression.

---

## Files Changed

- kernel/gfx/compositor.c:
  - added KWM_MAX_OCCLUSION_RECTS, rect_subtract(), mix_content();
  - composite_windows_in_rect() collects opaque occluders once, and each
    window's content is now drawn over clip minus front-opaque covers, with a
    bounded fallback to the full clip on overflow.

No changes to syscalls, KWM, libgui, libui, Rust, GHAL, base_blit_for_region
(Phase 18), or fully_opaque semantics.

---

## Limitations

- Benefit requires a validated fully-opaque window in FRONT of another window
  over the same dirty region. Today that means Rust/Slint or (after Phase 19)
  libgui/libui/desktop windows stacked over each other. A single window or
  non-overlapping windows gain nothing.
- Only the content canvas is culled; titlebar/shadow draws still run (they are
  overwritten by the front opaque content). This is a deliberate scope limit.
- Unlucky geometry can hit the 8-rect bound and fall back to full-clip drawing
  (safe, just less culling).
- Occlusion does not remove the per-window loop overhead, only the per-pixel
  content work.

---

## Status

COMPLETE — OCCLUSION CULLING VALIDATED.
- opaque windows occlude windows behind: yes (cases 2-10,14,17,18)
- transparent / non-opaque windows never occlude: yes (cases 13,15,16)
- z-order preserved: yes (draw order unchanged; only covered pixels skipped)
- partial / multi occluders, decorations: yes
- bounded rectangle fallback: yes (8-rect cap, full-clip fallback)
- no heap allocation in hot path: yes (fixed stack arrays)
- baseline RGB == optimized RGB, sentinel == 0: yes (18 cases + 1500 fuzz)
- Phase 18 regression: pass. Kernel / apps / Rust / ISO build: pass.
- QEMU boot + keyboard + mouse: pass.
- No FPS claim; composition-pixel reduction measured.
