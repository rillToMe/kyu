# Desktop Environment

`system/desktop/` is the default desktop implementation, built on top of
`libdesktop`. It is an implementation, not a framework: policy (wallpaper,
launcher, taskbar, notifications) lives here, while mechanism (the event loop,
events, surfaces) belongs to `libdesktop`.

Another desktop (`tests/target/test-desktop/`) can replace this directory
without touching the kernel or framework.

## Modules

| File | Responsibility |
| --- | --- |
| `main.cpp` | Composition only: `Application` + `DesktopShell` + `run()` |
| `desktop_shell.*` | `DesktopShell : Shell` — wallpaper, cursor, hover/preview, a 5-second rescan schedule, background-to-front draw order, Full vs Partial coordination |
| `wallpaper.*` | Reads `wallpaper=` from `/system/desktop.app` (falls back to a gradient); decodes and scales a PNG photo once, drawn as RLE into the window canvas (region-aware for Partial restore) |
| `launcher.*` | Scans `/apps` for `*.elf` plus manifest `<base>.app` (`name/color/hidden/icon`), icon grid, click → `spawn` |
| `app_icons.*` | Central icon resolution (custom → `default.png` → color box), 48px cache, `scale_icon`, `draw_px` (alpha blend onto the canvas) |
| `taskbar.*` | App icon slots (focus/hover), right-side system area (clock `HH:MM` + date), click → `activate` |
| `app_preview.*` | Static preview card on slot hover (icon + title + status) |
| `crash_notice.*` | Probes `poll_crash` once at startup, shows for `NOTIF_MS`, click → File Manager |
| `theme.hpp` | Metrics, palette, wallpaper list (private, not staged) |
| `sys_abi.hpp` | The single wrap of `userlib.h` on the implementation side |

State is explicit in objects (`Launcher`, `Taskbar`, `CrashNotice`,
`DesktopShell`, `Wallpaper`, `IconCache`, `AppPreview`); there is no mutable
global state.

## Layer Contract with the Compositor

The desktop window is **full-screen and opaque**. `gui_create_desktop()` fills
its canvas with an opaque background and declares it opaque, so the compositor
skips the base blit for the entire screen and overwrites `base_canvas`.

Consequence: drawing to `base_canvas` (for example, `sys_draw_image`) is never
visible. **All** pixels (wallpaper, icons, taskbar strip, preview cards,
notifications) are drawn into the window canvas via `Canvas::fill_rect` /
`draw_text`.

Implications:

- Since `libgui` has no draw-image primitive, a photo wallpaper is blitted as
  per-row RLE (one `fill_rect` per run of identical color) in
  `Wallpaper::draw_photo_into`; icons are alpha-blended per pixel via
  `Canvas::draw_px`.
- `png_decode` preserves ARGB8888 alpha (a=0 skipped, semi-transparent blended;
  the result canvas is always opaque so the compositor's opacity declaration
  stays valid). Icon assets may use transparency.
- A Partial render (taskbar strip + preview card) draws only that area; the
  preview card's old pixels are restored from the wallpaper via
  `draw_bg(region)`, not a full-screen redraw.
- The wallpaper buffer (about 8 MB at 1080p) comes from the standard allocator
  (`new (std::nothrow) uint32_t[...]`), not `sys_alloc`. The user heap grows
  on demand (additional arenas via `sys_alloc`), so an 8 MB request succeeds.

## Preserved Behavior

- Launcher with no hardcoding (`name/color/hidden/icon` from manifests).
- Taskbar discovery, focus, and activation (the title moves to the preview
  card).
- One-shot crash notice (clicking outside closes it without consuming the
  click).
- Damage: Full at startup / grid change / notice change; Partial on
  click/focus/hover/minute-change/preview; pointer motion outside the strip
  causes no repaint.
- Visual assets: `assets/wallpaper/*.png` (Limine modules → filesystem root),
  `assets/icons/default.png` (fallback) plus `demo.png` (a custom example via
  `icon=` in `manifests/widget_demo.app`).

## Building

```sh
./build.sh kyuzen-desktop                                  # default desktop
cmake -S . -B build/target -DKYUZEN_DESKTOP_APP=test-desktop  # swap implementation
./build.sh test && ctest --test-dir build/host -R test-desktop
```

The output always fills the boot slot `build/target/apps/desktop.elf`.

## Related Documentation

- [GUI Overview](README.md)
- [libdesktop](libdesktop.md)
- [Applications](../userspace/applications.md) — manifests
- [Graphics: Compositor](../graphics/compositor.md) — layer contract
