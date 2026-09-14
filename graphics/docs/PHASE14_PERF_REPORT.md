# Phase 14 — Opaque Window memcpy Fast-Path

Date: 2026-09-14
Status: COMPLETE — FAST-PATH VALIDATED

---

## Status

The opaque-window memcpy fast-path is implemented, builds on all targets
(kernel, Rust apps, hybrid ISO), boots in QEMU without panic, and its
composition output is validated pixel-identical (displayed RGB) against the
scalar baseline using a host integration test that #includes the real
compositor.c. Details and caveats below.

---

## Objective

Replace the scalar per-pixel alpha-test loop in composite_windows_in_rect()
(kernel/gfx/compositor.c) with a bulk row copy for windows whose canvas is
guaranteed fully opaque, while keeping the scalar path as the correctness
baseline for every non-guaranteed window.

---

## Phase 13 Basis

Phase 13 established (verified against source here):

- Pixel format XRGB8888 (include/display.h:14-18): low 24 bits = 0xRRGGBB,
  high byte = opacity mask (0x00 = transparent, non-zero = opaque).
- Compositor does alpha TEST, not blend (compositor.c:250):
  if (pixel high byte != 0) dst = pixel RGB.
- libgui/libui canvases: allocated by kmalloc, NOT zeroed — no whole-window
  opacity guarantee.
- Rust/Slint canvases: Vec::resize(px, Xrgb8888(0xFF000000)) — fully opaque
  by construction; every TargetPixel writer forces 0xFF
  (rust/kyuzen-gui/src/lib.rs).
- Phase 13 recommended: add a fully_opaque flag to kwm_window_t and use it for
  a compositor memcpy fast-path.

Re-verified in source this phase: the guarantee depends on the KERNEL-VISIBLE
canvas, not the userspace Vec. The kernel canvas is a separate display_buffer
surface (kwm.c display_buffer_create) and the kernel scans it before enabling
the flag, so the flag is established on the memory the compositor actually
reads.

---

## Existing Compositor Path

composite_windows_in_rect() iterates z, then all MAX_WINDOWS slots; for each
window intersecting the dirty rect it draws the shadow frame, then the content
rect (crect = window x/y + titlebar offset, size = window width/height), then
the WM titlebar. The content inner loop (pre-Phase-14) was:

    for each clipped row:
        for each pixel:
            pixel = src[xx]
            if (pixel high byte != 0) dst[xx] = pixel & 0xFFFFFF

z-order, shadow, titlebar, cursor, and dirty-rect clipping are unchanged by
Phase 14.

---

## Fully-Opaque Guarantee

The flag is NOT inferred from a language label. It is set only by a new
kernel path that validates the actual kernel canvas.

Trust model: userspace declares, kernel validates, then treats it as a
contract.

kwm_set_window_opaque(win_id) (kernel/gfx/kwm.c):
1. Rejects if the caller is not the window owner
   (owner_task == smp_current_task_id()) or the window/canvas is absent.
2. Takes a pointer to the canvas pixels and the pixel count under kwm_lock,
   then RELEASES the lock and scans every pixel for a zero high byte. Scan is
   outside the lock because the canvas can be large and kwm_lock is an IRQ-off
   spinlock; only the owner can write this canvas, so the race is benign.
3. If any pixel has alpha == 0, returns -1 and does NOT set the flag.
4. Otherwise re-takes the lock and sets fully_opaque = 1 (re-checking owner and
   active state).

Consequences:
- A false positive is rejected at declaration time by construction: the flag
  cannot be set unless the entire current canvas is alpha-nonzero.
- A false negative (reject) is always safe: the window stays on the scalar path.
- The flag is cleared on window creation and on slot free (kwm.c), so a reused
  slot never inherits a stale guarantee.

The declaration is a CONTRACT: after declaring, the owner must keep writing
non-zero alpha. The audited Rust/Slint writers do (every TargetPixel writer ORs
0xFF000000; canvas initialised to 0xFF000000). See Known Limitations for the
residual case of a contract-violating app.

---

## Implementation

Files and functions changed:

- kernel/gfx/kwm_internal.h: added `uint8_t fully_opaque;` to kwm_window_t.
  Documented as a performance hint, not a rendering semantic.
- kernel/gfx/kwm.c:
  - kwm_free_slot(): fully_opaque = 0 on free.
  - kwm_create_window(): fully_opaque = 0 on create.
  - kwm_create_desktop(): fully_opaque = 0 (desktop does not declare).
  - new kwm_set_window_opaque(): validation + set (above).
- kernel/syscall.c: syscall 67 -> kwm_set_window_opaque(rbx). Owner-only.
- rust/kyuzen-sys/src/lib.rs: SYS_KWM_SET_WINDOW_OPAQUE = 67 and
  kwm_set_window_opaque() wrapper.
- rust/kyuzen-gui/src/lib.rs: opaque_declared Cell; after draw_if_needed()
  succeeds, calls kwm_set_window_opaque once and stops retrying on acceptance.
  A persistent reject is a safe false negative.
- kernel/gfx/compositor.c: content inner loop branches on fully_opaque; adds
  #include <string.h> for memcpy; adds a compile-time FORCE_SCALAR_COMPOSITOR
  switch (off by default) for A/B baseline builds.

The optimization is localized to the compositor content copy plus the minimal
KWM/syscall plumbing required to establish the guarantee.

---

## Compositor Fast-Path

In composite_windows_in_rect(), inside the content intersection block:

    const int win_opaque = kwm_windows[w].fully_opaque;
    for each clipped row yy:
        src = canvas->pixels + sy*stride + (clip.x - win_x)
        dst = backbuffer + (clip.y+yy)*pitch4 + clip.x
        if (win_opaque)
            memcpy(dst, src, clip.width * sizeof(uint32_t))
        else
            existing scalar alpha loop

Preserved exactly:
- clipping: clip comes from rect_intersect(crect, r); only the window CONTENT
  region is bulk-copied.
- source/destination coordinates and strides: identical expressions to the
  scalar path; only the inner per-pixel loop is replaced.
- z-order: the window iteration is untouched; an opaque window does not stop
  processing of windows above it (no occlusion culling).
- shadow frame, titlebar, close button, title text, cursor, dirty rects:
  unchanged.
- scalar fallback: fully intact for every non-guaranteed window.

---

## Pixel Format / Destination Semantics

Source canvas and destination backbuffer are both XRGB8888. The scalar loop
writes `pixel & 0xFFFFFF` (high byte cleared). The fast path writes the full
32-bit source word (high byte = source alpha, guaranteed non-zero).

Display semantics: the destination is the backbuffer, which is uploaded to the
scanout (software: memcpy into the HW framebuffer; VirtIO: resource backing,
scanned out as X8R8G8B8). In XRGB8888 the high byte is a don't-care padding /
mask byte, not a displayed component; the HW scanout ignores it and nothing
reads back the backbuffer alpha. The displayed RGB is therefore identical.

The raw 32-bit words differ in that padding byte (0x00 in the scalar path,
0xFF/nonzero in the fast path). See Correctness Validation and Known
Limitations. This was NOT assumed — it is measured below.

---

## Correctness Validation

Method (MEASURED, host integration): a test harness in the OS temp directory
compiles the REAL kernel/gfx/compositor.c (plus kernel/display.c) for the host
with stubbed kernel globals, then calls the real composite_windows_in_rect()
twice on the same synthetic all-opaque 64x48 canvas — once with
fully_opaque = 0 (scalar baseline) and once with fully_opaque = 1 (fast path) —
and compares the resulting backbuffer.

Results:

- Case 1, canvas alpha == 0xFF everywhere:
  rgb_diff = 0 (zero differing displayed pixels)
  byte_diff = 3072 of 3072 (only the top/padding byte differs: 0xFF vs 0x00)
- Case 2, canvas alpha values non-zero but not 0xFF:
  rgb_diff = 0
- Case 3, one pixel with alpha == 0 and the flag wrongly forced on:
  rgb_diff = 1

Interpretation:
- For the guarded case (flag set only after kernel validation), displayed output
  is identical to the scalar baseline.
- Case 3 demonstrates that the fast path WOULD corrupt output if the flag were
  set without validation, confirming the kernel scan is load-bearing and that
  rejecting on any alpha == 0 is required.
- No differing displayed pixels; the only difference is the don't-care X byte.

QEMU runtime (MEASURED): QEMU 10.2.1 is available at
E:\Tools\msys2\mingw64\bin\qemu-system-x86_64.exe and WAS used (earlier phases
reported it unavailable because mingw64 was not on PATH). A boot smoke test of
the hybrid ISO in QEMU (-cpu max -m 1G -smp 4, disk.img + boot_image.iso,
serial to file) reached the login prompt with no panic, no exception, and no
fault in the serial log.

Not performed: an on-screen A/B framebuffer pixel diff between a fast-path build
and a FORCE_SCALAR_COMPOSITOR build. Across two separate boots the screen is not
deterministic (cursor blink, clocks, DHCP timing), so a raw framebuffer diff
would be unreliable. The deterministic host integration test above uses the
actual composition function and is the authoritative equivalence evidence.

---

## Performance Measurements

No fabricated numbers.

MEASURED: none for timing. QEMU is available but no in-guest cycle counters or
timestamps were added (instrumentation must be removed; none was added).

INFERRED (source-derived): the fast path replaces, per clipped pixel, one load +
one compare/branch + one masked store with a bulk memcpy over the same row. On
x86-64 the scalar loop is roughly one pixel per several cycles; memcpy moves
16-32 bytes per cycle. For an opaque window the per-pixel branch and masking
disappear. This is an instruction/work reduction on the content copy only.

Scope of the win (source-derived): only windows that declare and pass
validation benefit (currently Rust/Slint windows). libgui/libui/desktop windows
remain on the scalar path and are unchanged. The base blit, shadow, titlebar,
window iteration, GHAL upload and present costs (Phase 11) are unaffected.

Explicitly NOT claimed: any measured "Nx faster" figure. Phase 11's "roughly 3x
per byte" for scalar vs memcpy remains source-derived, not runtime-measured, and
Phase 14 did not measure a speedup either.

---

## Regression Testing

Builds (MEASURED, all exit 0):
- make clean; make -> myos.bin, no new warnings/errors
- make rust-apps -> hello-slint, control-center (release, 20.7s)
- make boot_image.iso -> ISO produced (5829 sectors), Limine BIOS install OK

Runtime (MEASURED): QEMU boot to login prompt, no panic.

Static/structural: scalar path preserved verbatim for fully_opaque == 0;
libgui/libui windows never set the flag (they never call syscall 67), so they are
provably on the scalar path. Window create/free reset the flag. z-order, clip,
stride, shadow and titlebar code paths are untouched.

Not exercised in QEMU this phase (would require interactive GUI automation):
opening a Rust/Slint window and visually confirming. The Rust declaration path is
covered by source inspection plus the kernel validation gate.

---

## Files Modified

- kernel/gfx/kwm_internal.h — kwm_window_t.fully_opaque
- kernel/gfx/kwm.c — flag reset on create/free; kwm_set_window_opaque()
- kernel/syscall.c — syscall 67
- kernel/gfx/compositor.c — memcpy fast-path + FORCE_SCALAR_COMPOSITOR
- rust/kyuzen-sys/src/lib.rs — syscall 67 constant + wrapper
- rust/kyuzen-gui/src/lib.rs — declare-opaque after first draw

---

## Files Not Modified

include/display.h, kernel/display.c (dirty regions), GHAL (graphics/ghal.c,
graphics/backend/*), VirtIO-GPU driver, apps/libgui.c, apps/libui.cpp,
apps/png.c, Slint renderer internals, KWM event routing, scheduler, heap,
syscall 31 and syscall 66, multi-region damage.

---

## Known Limitations

1. Padding-byte difference. The fast path preserves the source X/alpha byte
   (0xFF/nonzero) where the scalar path wrote 0x00. Displayed RGB is identical
   and the byte is ignored by XRGB8888 scanout, but a raw 32-bit framebuffer
   word comparison would show differences in that byte. "Pixel-identical" here
   means displayed pixels.

2. Contract after declaration. The kernel validates the canvas at declaration
   and does not re-scan on later uploads. An app that declares opaque and then
   writes an alpha == 0 pixel via syscall 31/66 would be memcpy'd and would show
   its own window incorrectly. This is a self-inflicted visual bug confined to
   that window; it cannot corrupt other windows or kernel memory. The audited
   Rust/Slint writers never do this. Re-validating every upload would move a
   per-pixel scan into the upload path and largely cancel the benefit — a
   deliberate tradeoff (ponytail: validated-at-declaration contract; per-upload
   revalidation if a non-cooperative opaque client ever appears).

3. New syscall. syscall 67 is added. Phase 13 showed no existing kernel-visible
   discriminator for "this window is opaque", so metadata had to be introduced;
   there is no way to set the flag without some interface. The call is
   owner-only and validates before granting.

4. Scope of benefit. Only declared/validated windows use the fast path. libgui,
   libui, desktop, and any window that fails validation remain scalar.

5. FORCE_SCALAR_COMPOSITOR remains in compositor.c as an inert, off-by-default
   compile switch used for the A/B baseline; it is not runtime instrumentation.

---

## Phase 15 Recommendation

Do not start a new rendering project. Two measured/source-derived items remain,
in priority order:

1. (Source-derived, Phase 11 #2) Dirty-region collapse to a single bounding box
   on update bursts (display.c:149) remains the largest work amplifier: 65+ small
   marks become one near-full-screen reprocess. That (multi-region damage) was
   explicitly deferred by Phase 7 and is still the highest-value next target.

2. Only after (1): reconsider the base blit of fully-covered opaque rects
   (Phase 11 duplicate-work #2), which the fast path does not address.

GPU acceleration remains deferred (Phase 11): the remaining costs are CPU-side.

---

## Acceptance Mapping

- fully_opaque metadata in kwm_window_t: yes
- only guaranteed-opaque windows flagged: yes (kernel validates the canvas)
- Rust/Slint recognised as opaque: yes (declares via syscall 67 after draw)
- libgui/libui stay non-opaque: yes (never declare)
- compositor bulk-copy fast path: yes
- source/destination semantics verified: yes (XRGB8888, X byte don't-care)
- dirty-rect clipping, z-order, decorations, scalar fallback: preserved
- displayed-pixel-identical to scalar: MEASURED rgb_diff 0 (cases 1-2)
- no syscall 31/66 ABI change; no multi-region change; no unrelated changes
- clean build (kernel + Rust + ISO): yes
- QEMU runtime test: yes (boot to login, no panic)
- no fabricated performance numbers: yes
