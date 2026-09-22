# Phase 19 — Opaque Guarantee Expansion

Date: 2026-09-14
Status: COMPLETE — C/C++ OPAQUE DECLARATION VALIDATED (BENEFIT LIMITED BY WORKLOAD)

---

## Objective

Let libgui and libui windows obtain the existing `fully_opaque` guarantee so the
Phase 14-18 compositor optimizations (opaque content memcpy + base-blit elision)
apply to C/C++ GUI windows, while keeping Rust/Slint working and adding no new
opacity system.

---

## Existing Opacity Architecture

Reused unchanged:
- `kwm_window_t.fully_opaque` (Phase 14) — a performance hint, not a render semantic.
- syscall 67 -> `kwm_set_window_opaque(win_id)` (kernel/gfx/kwm.c): owner-only,
  scans the WHOLE kernel-side canvas and sets the flag ONLY if every pixel has a
  non-zero alpha byte; otherwise it rejects and the window stays on the scalar
  path. This is the trust boundary and was NOT weakened.
- Compositor consumption (Phases 14/16/17/18) is unchanged.

The only missing piece was that no C userspace wrapper existed and libgui never
called it.

---

## libgui Integration

libs/core/libgui.c — after the initial full-canvas paint + upload, each window
declares itself opaque:
- `gui_create_window()`: fills the whole canvas `0xF5F5F5` (opaque via
  `color | 0xFF000000`), uploads it with syscall 31, then calls
  `sys_kwm_set_window_opaque(win->win_id)`.
- `gui_create_desktop()`: fills the whole screen `0x1E293B` opaque, uploads,
  then declares. A full-screen opaque cover is the strongest case: every dirty
  region on screen is covered, so the base copy is elided for all of them.

Declaration happens ONCE at window creation — not per frame, not per widget
update.

---

## libui Integration

No libui change was needed. libui's `Window` is built on `gui_create_window()`
(libs/gui/widget/, dulu apps/libui.cpp), so it inherits the opaque declaration automatically. All libui
canvas writers are opaque:
- `Painter::rect/text` -> gui_draw_* -> `color | 0xFF000000`.
- `Painter::blend` -> `aa_mix(...) | 0xFF000000`.
- `Painter::image` -> PNG pixels from libs/media/png.c, which forces `0xFF000000`
  on every decoded pixel (png.c:45).
- `Painter::shadow` -> blend with nonzero alpha -> `| 0xFF000000`.

There is no libui path that writes an alpha == 0 pixel into a window canvas, so
no invalidation hook is required (see Opacity Invalidation).

---

## Canvas Initialization

Verified: kmalloc/sys_alloc canvases are NOT zero-initialized, so a window is
only safe to declare opaque after its ENTIRE canvas has been painted opaque.
libgui paints the whole canvas in `gui_create_window`/`gui_create_desktop`
(one-time O(n) fill, not per frame) before declaring. This closes the Phase 12/13
"uninitialized pixel" blocker without any new initialization strategy.

I also checked every direct canvas writer: libs/gui/widget/include/core/painter.hpp (dulu apps/libui.cpp) writes at lines 183
(PNG, forced opaque) and 197 (`| 0xFF000000`); system/desktop/ uses only
gui_draw_rect/gui_draw_text; apps/badptr.c writes 0xFF204060. No writer
emits alpha == 0.

---

## Opacity Invalidation

Not required in this phase: there is no C/C++ code path that changes a declared
window from opaque to transparent.
- Every libgui/libui primitive forces alpha 0xFF.
- The PNG decoder forces alpha 0xFF.
- No "clear to transparent" or raw alpha write exists.

Per the phase guidance, no speculative dependency-tracking / invalidation
framework was added. If a future transparent-write path is introduced, it must
call a (to-be-added) invalidation; the kernel validation already guarantees the
initial declaration is honest. KWM resets `fully_opaque = 0` on window create
and on slot free, so a recreated window never inherits a stale flag.

---

## Kernel Validation

Unchanged from Phase 14. Verified by a new host test that compiles the REAL
kernel/gfx/kwm.c (`kwm_set_window_opaque`) with spinlocks macro-mocked:

    opaque canvas (alpha 0xFF)      -> declare=0  fully_opaque=1   (accepted)
    one alpha==0 pixel              -> declare=-1 fully_opaque=0   (REJECTED)
    alpha 0x80 canvas (non-zero)    -> declare=0  fully_opaque=1   (accepted)
    non-owner opaque                -> declare=-1 fully_opaque=0   (rejected)
    inactive window                 -> declare=-1                  (rejected)

So a userspace mis-declaration (transparent canvas) is rejected by the kernel.

---

## Host Harness

Two host harnesses, both against REAL kernel code (locks macro-mocked; base-blit
bytes measured with link-time --wrap=viewport_render, no production
instrumentation):

1. kernel-validation harness: compiles real kwm.c (results above).
2. compositor harness: real compositor_flush() vs a forced-base-blit baseline;
   backbuffer pre-filled with a sentinel; RGB compared.

Compositor scenes model a C/C++ opaque window now declaring opaque:

MEASURED (host), W=400 H=300. "Before" = window not declared (base blit kept);
"After" = window declared opaque (Phase 19):

    Scene                                rgb_diff sentinel  BeforeB  AfterB  Reduction
    libgui opaque covers dirty               0        0        40000       0     100%
    libui opaque covers dirty                0        0       100800       0     100%
    two C/C++ opaque windows                 0        0       180000       0     100%
    Rust + libgui opaque mixed               0        0       216000   36000      83%
    non-opaque C/C++ window (no guarantee)   0        0        90000   90000       0%
    opaque below transparent (libgui)        0        0       144000       0     100%
    small dirty in big opaque window         0        0        12000       0     100%
    partial cover (libgui left half)         0        0       180000  120000      33%

    rgb_diff == 0 and sentinel == 0 in every scene.

The Phase 18 harness (14 cases + 400-scene fuzz) was re-run unchanged:
0 failures, 0 sentinel — the compositor algorithm was not touched.

Note: the compositor does not know a window's origin; "libgui/libui/Rust" scenes
differ only in geometry and the `fully_opaque` flag. The Phase 19 change is what
now SETS that flag for libgui/libui windows at creation.

---

## Performance Results

| Scene | Before (base bytes) | After (base bytes) | Reduction |
|------|--------|-------|-----------|
| libgui opaque covers dirty | 40000 | 0 | 100% |
| libui opaque covers dirty | 100800 | 0 | 100% |
| two C/C++ opaque windows | 180000 | 0 | 100% |
| Rust + libgui mixed | 216000 | 36000 | 83% |
| non-opaque / rejected window | 90000 | 90000 | 0% |
| opaque below transparent | 144000 | 0 | 100% |
| small dirty in big opaque | 12000 | 0 | 100% |
| partial cover (left half) | 180000 | 120000 | 33% |

These are base_canvas -> back_buffer BYTES, measured on the host against the real
compositor_flush. No FPS or CPU-time number is claimed (not measured).

---

## QEMU Validation

QEMU 10.2.1 (E:\Tools\msys2\mingw64\bin). Hybrid ISO booted with -display none,
serial to file, monitor on TCP:
- no panic / exception / fault;
- login screen renders (1280x800);
- sendkey changed the framebuffer (CRC) and the login flow advanced
  (Username -> Password) — keyboard path works;
- mouse_move changed the framebuffer (CRC) — cursor path works.

Limitation: the persisted disk.img has an unknown login password, so running a
libgui/libui app on-screen to watch the opaque declaration in the live
compositor was not automatable. The declaration path is validated by (a) the
kernel-validation host test on the real kwm.c and (b) the compositor host harness
on the real compositor_flush; QEMU confirms the whole image still builds, boots
and renders without regression.

---

## Safety / Correctness

- Kernel trust boundary intact: declaration is owner-only and requires every
  canvas pixel to have alpha != 0; a transparent canvas is rejected (host test).
- Skipping the base copy is provably pixel-neutral: the covered sub-rect is
  written by a validated opaque window during composition, so the final
  framebuffer is identical (rgb_diff == 0 in every harness scene, sentinel == 0).
- Failure to declare is safe: the window simply keeps the scalar path and the
  base blit.
- No new opacity metadata, no ABI change (reuses syscall 67), no compositor
  change, no per-frame syscall, no per-frame full-canvas memset.
- Rust/Slint unaffected (its own declaration path is untouched; compositor
  re-run proves no regression).

---

## Limitations

- Performance benefit depends on opaque windows actually covering dirty regions.
  In the current layouts the only opaque producers were Rust/Slint; libgui/libui
  windows are often small and frequently not overlapping large dirty areas, so
  the day-to-day gain is expected to be modest. The full-screen DESKTOP window
  (now opaque) is the exception and removes the base copy for essentially every
  dirty region while the desktop is up.
- No invalidation hook exists because no C/C++ transparent-write path exists
  today; adding one later requires a small invalidation call.
- On-screen QEMU validation of a live libgui/libui window was not automatable
  (unknown login password); logic validated on the host against real kernel code.

---

## Files Changed

- include/userlib.h — declared `int sys_kwm_set_window_opaque(int win_id);`.
- libs/core/userlib.c — implemented the syscall 67 wrapper.
- libs/core/libgui.c — call `sys_kwm_set_window_opaque()` after the initial opaque
  fill + upload in `gui_create_window()` and `gui_create_desktop()`.

(Other files shown modified in git predate Phase 19.)

No change to: kernel/gfx/compositor.c, kernel/gfx/kwm.c, kernel/display.c,
kernel/syscall.c, GHAL, VirtIO-GPU, Rust/Slint sources, libui.
