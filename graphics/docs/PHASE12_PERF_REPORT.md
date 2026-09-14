# Phase 12 Report — Compositor Opaque Fast Path

Date: 2026-09-14
Status: INVESTIGATION COMPLETE — Fast-path NOT IMPLEMENTED

## 1. Objective

Optimize `composite_windows_in_rect()` (`kernel/gfx/compositor.c:241-252`) using a bulk row-copy when window content is fully opaque, instead of the scalar per-pixel read/test/write loop.

## 2. Current Composition Loop

```c
for (uint32_t xx = 0; xx < clip.width; xx++) {
    uint32_t pixel = src[xx];
    if (pixel >> 24) dst[xx] = pixel & 0xFFFFFF;
}
```

- Pixels are XRGB8888.
- Alpha byte (bits 31-24): 0x00 = transparent (skip), non-zero = opaque (write RGB).
- Branch predicts perfectly for mostly-opaque content.

## 3. Pixel Format & Alpha Semantics

Verifiable from source:

- `include/display.h:14-18`: "XRGB8888. Low 24 bits = 0xRRGGBB. High byte = opacity mask: 0x00 = transparent, non-zero = opaque."

## 4. Critical Finding: Window Canvas Alpha Content

NOT GUARANTEED OPAQUE.

- Window canvases allocated via `sys_alloc` -> `kmalloc` (kernel/heap.c), which does NOT zero memory.
- Initial content is malloc garbage. Could include 0x00000000 (fully transparent).
- Apps only paint pixels they draw. Unpainted pixels remain garbage.
- If a dirty rect includes unpainted pixels with alpha=0, they are skipped by the per-pixel loop, preserving background — correct behavior.

## 5. Fast-Path Safety Analysis

A bulk memcpy would COPY the source bytes (including alpha byte) to destination. If source contains ANY alpha=0 pixel:
- memcpy writes 0x00RRGGBB to that destination location.
- The compositor should NOT have written there (transparent = leave as-is).
- Result: visual corruption (transparent regions become black).

Therefore, a fast-path is ONLY safe if the CLIPPED SOURCE ROWS are GUARANTEED to have alpha!=0 for every pixel.

## 6. Can We Prove a Region is Opaque?

Without metadata: NO.
- Window creation: canvas allocated, NOT zeroed, then app paints partway.
- libgui fills entire canvas on creation (`_lgui_fill_rect(0,0,w,h,0xF5F5F5)`).
- But apps can create windows, exit, delete only part. Unwritten memory persists.

Options (all beyond `compositor.c` changes):
A. Window-level opaque flag (new field in kwm_window_t).
B. Row-level opacity metadata (extra memory per row).
C. App asserts after fill (new syscall 67+?).

## 7. Conclusion: DEFER Fast-Path

The proposed optimization proved UNSAFE under current constraints:
1. Canvas memory is NOT zeroed → contains garbage → could mix opaque and transparent pixels arbitrarily.
2. The per-pixel loop correctly handles this case (transparent pixels skip write).
3. A blind memcpy would fail for any window with unpainted/uninitialized pixels in its dirty region.

The optimization cannot be implemented solely within the compositor without additional infrastructure.

## 8. Alternative Optimizations (Retained)

The scalar loop already has predictable branching for opaque content. No micro-optimization available within current architecture.

## 9. Decision: DEFER (not implement)

Reason: Safety requires opacity metadata that is not present. Implementing without verification would cause visual corruption.

Next-step recommendation (Phase 13+): Add an `KWM_WIN_OPAQUE` flag or dirty-region "opaque hint" so the compositor can safely fast-path fully-opaque contents.

## 10. Re-Test Rationale

After metadata is added:
- Verify no pixel difference between fast-path memcpy and scalar loop.
- Test: opaque windows, overlapping windows, partial paints, uninitialized memory scenarios.

## 11. Acceptance Criteria

[ x ] Pixel/format semantics verified from source (XRGB8888, high byte = mask)  
[ x ] Window canvas initialization audited (kmalloc, not zeroed)  
[ x ] Drawing paths audited (all libgui/libui/PNG write opaque; no transparent-path writes found)  
[ x ] Fast-path safety demonstrated impossible without metadata  
[ x ] No production code changes introduced  
[ x ] Repository state verified (nothing modified)

---

**Recommendation for Phase 12:** No optimization implemented. Fast-path unsafe. Defer pending Phase 13+ architecture addition (opacitiy metadata).

Note: Per Phase 11, GPU acceleration also deferred. CPU remains the relevant focus.
