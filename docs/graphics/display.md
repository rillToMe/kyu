# Display & Primitives

This document describes the display abstraction (`DisplayBuffer`, `Viewport`,
`display_mode_t`) and the low-level drawing primitives.

## Display Mode

`display_mode_t` is the single source of truth for screen geometry:

```c
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch_bytes;
    uint32_t bpp;
    uint32_t format;
} display_mode_t;
```

The only supported format is `DISPLAY_FMT_XRGB8888` (32 bpp little-endian
`0x00RRGGBB`). `DISPLAY_MAX_DIM` = 8192 bounds all dimensions.

### Boot validation

`display_boot_init(fb, w, h, pitch_bytes, fmt)` validates the Limine
framebuffer:

- Non-NULL, non-zero dimensions, each ≤ 8192.
- `pitch >= width * 4`.
- 32 bpp, RGB memory model, 8/8/8 channel masks, shifts R=16, G=8, B=0.

The kernel halts if the framebuffer does not meet these requirements. Runtime
mode-setting is available only from a backend that advertises
`GHAL_CAP_MODE_SET` (`display_set_mode`).

### Buffer allocation

`display_alloc_buffers()` allocates `base_canvas` and `backbuffer`, each
`pitch_bytes * height` bytes (pitch-aware, overflow-checked), zeroes them, and
wraps static `DisplayBuffer` structures (`g_screen_db`, `g_back_db`,
`g_fb_db`). It is idempotent.

## DisplayBuffer

```c
typedef struct {
    uint32_t            width, height, stride;
    ColorFormat         format;
    uint32_t           *pixels;
    uint8_t             owns_pixels;
    struct DirtyRegionList *dirty;
} DisplayBuffer;
```

- `stride` is pixels per row.
- `dirty == NULL` means the buffer does not track damage (the screen buffers
  use the locked `screen_mark_dirty` instead).
- `display_buffer_create` allocates and owns its pixels; `display_buffer_wrap`
  borrows an existing buffer.

## Viewport

```c
typedef struct {
    Rect           bounds;
    int32_t        scroll_x, scroll_y;
    DisplayBuffer *source;
} Viewport;
```

`viewport_scroll` adjusts the offset and `viewport_render` copies the visible
region. The render hot path computes the clip once and then copies one row at a
time with `memcpy`.

## Rectangles

```c
typedef struct { int32_t x, y; uint32_t width, height; } Rect;
```

Helpers `rect_intersect` and `rect_union` are 64-bit safe.

## Color

```c
typedef uint32_t Color;
typedef enum { COLOR_FORMAT_XRGB8888 = 0 } ColorFormat;
```

The native format is XRGB8888: the low 24 bits are `0xRRGGBB` and the high byte
is an opacity mask (0 = transparent, non-zero = opaque). Every layer uses this
format with no mid-pipeline conversion.

## Primitives

Low-level drawing lives in `kernel/gfx/fb.c` with declarations in
`include/gfx.h`. The screen buffers are the globals `fb_ptr`, `backbuffer`, and
`base_canvas` (stride = `pitch_bytes / 4`).

| Primitive | Description |
| --- | --- |
| `draw_pixel(x, y, color)` | Single pixel |
| `draw_rect(x, y, w, h, color)` | Filled rectangle |
| `draw_image(sx, sy, w, h, buffer)` | Blit (alpha > 0 → `pixel & 0xFFFFFF`) |
| `draw_char(c, x, y, color)` | 8×16 bitmap glyph |
| `draw_string(...)` | Text run |

Primitives on the screen path self-mark dirty (`screen_mark_dirty`), so callers
do not need to track damage manually. A helper `screen_put` writes without
marking.

### Compositor entry points

| Function | Purpose |
| --- | --- |
| `screen_mark_dirty(x, y, w, h)` | Record damage |
| `compositor_flush()` | Build and present a frame |
| `compositor_panic_cursor_off()` | Hide the cursor on panic |
| `kwm_set_cursor(kind)` | Select the cursor shape |

## Font

The kernel/TTY/panic paths use a bitmap font (`font8x16`, 8×16 glyphs). Rich
font rendering via FreeType is userspace-only; see
[Shared Libraries](../libraries/shared.md).

## Related Documentation

- [Graphics Overview](README.md)
- [Compositor](compositor.md)
- [Window Manager](window-manager.md)
- [Shared Libraries](../libraries/shared.md) — color library and text rendering
