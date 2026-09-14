# Phase 13 — Opacity Metadata & Guarantee Audit

Date: 2026-09-14
Status: COMPLETE — Audit findings documented.

---

## 1. Executive Summary

**Key Finding: Rust/Slint windows have a guaranteed opaque canvas. libgui/libui windows do NOT.**

- Rust/Slint canvases are explicitly initialized via `canvas.resize(px, Xrgb8888(0xFF000000))` — fully opaque black.
- libgui/libui canvases use `kmalloc` which does NOT zero memory; apps must paint entire canvases.
- PNG decoder forces opaque (0xFF alpha).
- libui Painter forces opaque via `| 0xFF000000`.
- **No safe page-level fast-path exists for libgui/libui without opacity metadata.**
- **A Rust-window-only fast-path CAN be implemented safely (full-canvas opaque).**

**Recommendation for Phase 14: IMPLEMENTABLE**
- Add minimal opacity metadata: `KWM_WIN_OPAQUE_WINDOW` flag.
- Set it for Rust windows (trusted, guaranteed by Vec::resize).
- Optionally set it for libgui/desktop windows after first full paint.
- Do NOT set it for libui/PNG windows until runtime verification proves safety.

---

## 2. Existing Pixel/Alpha Semantics

Verified from `include/display.h:14-18`:

    XRGB8888
    - bits 31-24 = opacity mask: 0x00 = transparent (skip write), non-zero = opaque
    - bits 23-0 = 0xRRGGBB color

Verified from `kernel/gfx/compositor.c:250`:

    if (pixel >> 24) dst[xx] = pixel & 0xFFFFFF;

- This is an ALPHA TEST, NOT BLEND.
- alpha=0: skip, preserve destination.
- alpha!=0: replace destination with RGB.

---

## 3. Canvas Initialization Audit

| Path | Allocation | Zeroed? | Initial Content |
|------|------------|---------|-----------------|
| libgui (sys_alloc -> kmalloc) | kmalloc | NO | garbage |
| libui (same) | kmalloc | NO | garbage |
| Rust/Slint (Vec::resize) | allocator | YES (to given value) | 0xFF000000 (opaque black) |
| GHAL surface (sw_surface_create) | kmalloc + memset | YES | zeros |
| GHAL scanout | wraps HW fb | N/A | hardware |

**Conclusion**: Only Rust/Slint canvases have a predictable initial opacity guarantee.

---

## 4. Pixel Writer Audit

All paths except uninitialized memory force opaque:

| Writer | Source | Alpha Output | Classification |
|--------|--------|--------------|----------------|
| gui_draw_rect | libgui | `solid = color | 0xFF000000` | OPAQUE GUARANTEED |
| gui_draw_text | libgui | `solid = color | 0xFF000000` | OPAQUE GUARANTEED |
| _lgui_fill_rect | libgui initial | `0xFFF5F5F5` | OPAQUE |
| libui Painter::rect | libui | `color \| 0xFF000000` | OPAQUE GUARANTEED |
| libui Painter::blend | libui | `aa_mix(...) | 0xFF000000` | OPAQUE GUARANTEED |
| libui Painter::image (PNG) | libui | png.c forces 0xFF alpha | OPAQUE |
| Rust TargetPixel::blend | Rust | `0xFF00_0000 \| RGB` | OPAQUE GUARANTEED |
| Rust TargetPixel::from_rgb | Rust | `0xFF00_0000 \| RGB` | OPAQUE GUARANTEED |
| Rust TargetPixel::background | Rust | `0xFF00_0000` | OPAQUE |

**Uninitialized memory**: libgui/libui canvases contain kmalloc garbage before paint.

---

## 5. Safety Analysis for Fast-Path

### Candidate: Whole-window opaque

If we knew a window canvas was fully opaque (all pixels alpha=0xFF), the inner composition loop could use memcpy:

    if entire clipped region opaque:
        memcpy(dst_row, src_row, row_bytes)
    else:
        per-pixel alpha loop

### Rust/Slint: SAFE for whole-window fast-path

- Vec::resize fills with 0xFF000000 (opaque).
- Slint renderer only modifies dirty regions; unmodified pixels stay opaque.
- Any painted pixel is opaque.
- **Therefore: Rust canvas = fully opaque region-by-region patch, rest is opaque background.**

### libgui/libui: NOT safe without metadata

- Canvas allocated unfilled.
- Application paints only damaged region.
- Unpainted pixels = kmalloc garbage = could be any value, including 0x00000000 (alpha=0).
- Fast-path memcpy would corrupt display by writing transparent pixels over background.

---

## 6. Option A — Window-Level Opacity Metadata

**Design**: Add `uint8_t fully_opaque` flag to `kwm_window_t`.

**How set**:
- KWM sets it ON for Rust windows (Vec::resize guarantee).
- KWM could set it for libgui after verifying full-canvas paint OR on app request via new syscall.
- KWM does NOT set it for libui/PNG windows without explicit app declaration.

**How used**:
- In `composite_windows_in_rect`, before per-pixel loop:
    if (win->fully_opaque && rect_fits_fully_in_canvas_content) memcpy rows.

**Update cost**: None (flag static per window).

---

## 7. Option B — Row-Level Metadata

Rejected: Per-row array of 1920 bytes is small but adds complexity. Rust already has full-window guarantee, making row metadata unnecessary for our use case.

---

## 8. Option C — Opacity Initialization

Initialize all kmalloc-based canvases to 0xFF000000 instead of leaving garbage.

**Cost**: 8MB memset per full-screen window (1920x1080x4 = ~8MB). On window creation, adds latency. Frequent for multi-window setups.

**Why not chosen**: The cost outweighs benefit; Rust’s Vec::resize already proves this pattern works only where Rust controls allocation.

---

## 9. Option D — Scan-Then-Memcpy

Scan row for any alpha=0 pixel. If none found, memcpy.

**Cost**: 2 passes per row (scan + memcpy) vs 1 pass (current loop). Slower unless memcpy has massive throughput advantage (unlikely at CPU speeds).

**Rejected**: Does not improve performance.

---

## 10. Metadata Ownership & Trust Model

**Rust/Slint**: Trusted — Vec::resize guarantees opaque initialization. No syscall needed.

**libgui**: KWM derived. Could be set after confirming app paints full canvas (external visual inspection or post-creation assertion).

**libui**: Unreliable. Apps may leave regions unpainted. No safe way to trust userspace.

---

## 11. Damage Interaction

- syscall 66 `kwm_update_window_rect` receives a rect from userspace.
- If `fully_opaque=true`, the entire clipped region is assumed opaque.
- Compositor fast-path: memcpy the clipped region directly.

---

## 12. Recommendation

**Phase 14 Implementation Available.**

1. Add `uint8_t kwm_fully_opaque` to `kwm_window_t`.
2. Initialize to 0.
3. Rust window creation path sets it to 1.
4. In `composite_windows_in_rect`, when region entirely within canvas:
    if (win->fully_opaque) { memcpy row; skip per-pixel loop; }

No syscall ABI change needed (flag is internal to kernel).

---

## 13. Remaining Bottlenecks (Post-Fast-Path)

Even with opaque fast-path for Rust windows:
1. Dirty-region collapse still dominant.
2. Window iteration still O(N).
3. VirtIO overhead for small rects.

Defer these to Phase 15+.

---

## 14. Files Inspected

`include/display.h`, `kernel/gfx/compositor.c`, `kernel/gfx/kwm.c`, `kernel/display.c`, `apps/libgui.c`, `apps/libui.cpp`, `apps/png.c`, `rust/kyuzen-gui/src/lib.rs`, `kernel/heap.c`, `graphics/backend/software.c`, `include/aa_math.h`

---

## 15. Files Modified

**None.** This was an audit-only phase.

---

## 16. Acceptance Criteria

[ x ] Pixel format verified (XRGB8888, high byte = opacity mask)
[ x ] Alpha semantics verified (alpha=0 skip, alpha!=0 replace)
[ x ] Canvas initialization audited (Rust: opaque, others: garbage)
[ x ] Pixel writers audited (all write opaque; only initial state differs)
[ x ] Fast-path safety proven for Rust, unsafe for others without metadata
[ x ] No production code changed
Repository state verified — clean.
