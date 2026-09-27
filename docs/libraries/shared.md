# Shared Libraries

KyuzenOS provides several userspace libraries shared across applications: the
color library, the media/PNG decoder, and the text/FreeType rendering library.

## Color Library

Location: `libs/gui/color/`. A zero-dependency color library usable in both the
kernel and userspace.

| Header | Contents |
| --- | --- |
| `include/color_types.h` | `color_t`, `color_format_t`, `COLOR_RGB`, `COLOR_RGBA`, `COLOR_HEX`, conversions |
| `include/color_blend.h` | `color_blend_alpha`, `color_blend_span`, `color_div255` |
| `include/color_space.h` | HSL/HSV types and conversions |
| `include/color_utils.h` | Palette, `color_darken`/`color_lighten`, `color_get_contrast_text` |

### Representation

```c
typedef struct { uint8_t r, g, b, a; } color_t;
```

Four bytes, field order R, G, B, A, non-premultiplied straight alpha. `a == 0`
is transparent; `a != 0` is opaque (matching the display mask model).

`color_format_t` (`FORMAT_ARGB`, `FORMAT_RGBA`, `FORMAT_ABGR`, `FORMAT_BGRA`)
is for serialization only. The framebuffer's `DISPLAY_FMT_XRGB8888` does not
match `color_t` directly; conversion happens at each boundary.

### Blending

`color_blend_alpha(src, dst)`:

- `src.a == 0` → dst unchanged.
- `dst.a == 0` → src with `a = 255`.
- `src.a == 255` → src.
- Otherwise: `out = (src * src.a + dst * (255 - src.a)) / 255`, `out.a = 255`.

`color_div255(x) = (x + 1 + (x >> 8)) >> 8` (exact for `x < 65536`, no divide).

### Hex syntax

The adopted constant is `COLOR_HEX(0xRRGGBB)` — a braced-list initializer
(`{r, g, b, 255}`) that is valid in a constant expression, zero-overhead, and
works in `static const` tables. Its semantics are `0xRRGGBB` (not
`0xAARRGGBB`); use `COLOR_RGBA` for non-255 alpha. The runtime form is
`color_hex(0xRRGGBB)`.

A raw `#1E1E1E` literal is impossible in C/C++ (a `#` at expression position
is a stray preprocessing token), so it was rejected.

### HSL/HSV

`color_hsl_t` (`h 0..359, s 0..255, l 0..255`) and `color_hsv_t`. Conversions
are integer-based; a full RGB → HSL/HSV → RGB round trip is accurate to within
±3 per channel.

## Media / PNG Decoder

Location: `libs/media/` (`png.c`, `media.c`). A shared image decoder used by the
image viewer, gallery, and desktop.

- Backed by a trimmed **stb_image** with PNG and BMP only. JPEG and GIF are
  deliberately disabled (the JPEG IDCT is float, and there is no libm).
- Content sniffing is by bytes, not file extension.
- `png_decode` converts RGBA to XRGB8888; decoded pixels carry alpha, and the
  output canvas is written opaque so the compositor's opacity declaration stays
  valid.
- Neutral aliases `image_decode`/`image_free` (`include/media.h`) are provided
  for new applications; `png_decode`/`png_free` are retained for existing
  callers.

### Limits

| Limit | Value |
| --- | --- |
| Max file size | 8 MiB (`UC_MAX_FILE`) |
| Block cache | 1 MiB (256 blocks) |

A file larger than 8 MiB cannot be read (the read is rejected); files larger
than 1 MiB bypass the cache and are re-read from disk each open.

## Text / FreeType Rendering

Location: `libs/text/` (`kzfont.c`, `kzraster_ft.c`). A userspace text
rendering library backed by vendored **FreeType 2.14.3**
(`third_party/freetype/`, FreeType License).

- Modules enabled: truetype, sfnt, smooth, psnames.
- The kernel, TTY, and panic paths keep the bitmap `font8x16`; FreeType is
  **not** linked into the kernel.
- No second allocator: the application's heap (`sys_alloc`/`free`/`realloc`) is
  injected via `FT_MemoryRec`.
- A font is loaded from a borrowed in-memory blob (`FT_New_Memory_Face`), not
  from the filesystem.
- Glyph cache: a 256-slot open-addressing table keyed by (codepoint, size); on
  overflow it is fully dropped and refilled. Sizes range 1..256 px.
- UTF-8 decoding supports 1–4 bytes; invalid sequences map to U+FFFD. There is
  no shaping (no HarfBuzz).
- Fallback on failure: the caller falls back to the bitmap font or a U+FFFD
  box; the library never panics.

### Fonts

Bundled in `assets/fonts/`: DejaVu Sans, Inter Regular, Noto Sans Mono
(Regular/Bold), and Noto Sans Adlam (used as a missing-glyph test). All are
non-variable TTFs, embedded via `ld -b binary`.

### Policy

Low-level surfaces (kernel, shell, panic, TTY, KWM titles, compositor) use the
bitmap font; high-level surfaces (desktop launcher labels, Settings, fontdemo)
use FreeType via `libs/text`. The active font is selected through the registry
(`libs/text/include/kzfonts.h`) and the `/font.ui` config file.

## Related Documentation

- [Graphics: Display & Primitives](../graphics/display.md) — the color contract
- [GUI: Widget Toolkit](../gui/widget-toolkit.md) — theme colors
- [Desktop Environment](../gui/desktop.md) — icon and wallpaper rendering
- [C SDK](c-sdk.md), [C++ SDK](cpp-sdk.md)
