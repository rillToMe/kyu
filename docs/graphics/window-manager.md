# Window Manager (KWM)

The KyuzenOS window manager (KWM) lives in `kernel/gfx/kwm.c` with the ABI in
`include/kwm_abi.h` and internal definitions in `kernel/gfx/kwm_internal.h`.

## Overview

| Property | Value |
| --- | --- |
| Max windows | `MAX_WINDOWS` = 16 |
| Max window dimension | `KWM_MAX_DIMENSION` = 4096 |
| Max canvas bytes | `KWM_MAX_CANVAS_BYTES` = 16 MiB |
| Title bar height | `KWM_TITLEBAR_H` = 32 |
| Close button width | `KWM_CLOSE_BTN_W` = 46 |
| Corner radius | `KWM_CORNER_R` = 8 |
| Shadow margin | `KWM_SHADOW_MARGIN` = 6 |
| Edge alpha | `KWM_EDGE_ALPHA` = 70 |

## Window Structure

```c
typedef struct {
    uint8_t        active;
    int32_t        x, y;          // frame top-left (includes title bar)
    uint32_t       width, height; // content size (== canvas)
    DisplayBuffer *canvas;        // owned
    uint32_t       z_index;
    int32_t        owner_task;    // -1 = none
    uint32_t       flags;         // KWM_WIN_DESKTOP
    uint8_t        fully_opaque;  // validated performance hint
    char           title[32];
} kwm_window_t;
```

Global state: `kwm_windows[MAX_WINDOWS]`, `next_z_index` (starts at 1),
`kwm_lock`, `focused_win_id` (-1 = none), `hovered_close_win`.

## Ownership

A window is owned by the task that created it (`owner_task`). Only the owner
may write to its canvas:

- `kwm_update_window`, `kwm_update_window_rect`, `kwm_set_window_opaque`, and
  `kwm_set_title` all verify `owner_task == smp_current_task_id()`.
- Window ownership is task-id based and never trusts the UID.
- When a task exits, `kwm_destroy_windows_of(task_id)` tears down its windows.

The desktop is a special window flagged `KWM_WIN_DESKTOP` (`0x1`); it is
full-screen, opaque, and at the bottom of the z-order.

## Z-Order

`z_index` orders windows; higher is nearer the front. The desktop stays at
`z = 0`.

Because `z_index` grows monotonically on create / activate / bring-to-front /
cycle-focus, it is periodically re-normalized to prevent unbounded growth:

`kwm_normalize_zindex_locked()` (called with `kwm_lock` held, when
`next_z_index > MAX_WINDOWS`) insertion-sorts active non-desktop windows by
`z_index`, reassigns `0..N-1` preserving order, and sets
`next_z_index = N + 1`.

This also prevents a `uint32` wrap from colliding with the desktop's `z = 0`.

## Focus

- `focused_win_id` is set on window creation and on click-to-focus.
- `kwm_refocus_locked()` picks the highest-z active non-desktop window after
  the focused window is destroyed; if none remains, `focused_win_id = -1` and
  the keyboard returns to the TTY.
- `kwm_cycle_focus_locked()` implements Alt-Tab: the next window by z-order,
  wrapping around.

The focused window owns the keyboard; `kwm_route_keyboard` delivers key events
to the focused window's task event queue.

## Mouse Handling

`kwm_process_mouse(mouse_x, mouse_y, left_down, left_up)`:

1. Update the close-button hover state.
2. On `left_up`, end any drag.
3. While dragging, move the window, clamped to the display, invalidating both
   the old and new frame areas.
4. On `left_down`: hit-test; click-to-focus and bring-to-front; if the click is
   on the close button, push an `EVENT_WIN_CLOSE` event to the owner; otherwise
   start a drag.

A click on the desktop does not refocus or bring-to-front; the event passes to
the application.

> **Correctness note:** `dragged_win_id` is read and re-validated **inside**
> `kwm_lock` before dereferencing `kwm_windows[dragged_win_id]`. Reading it
> before the lock allowed a race with window destruction that produced an
> out-of-bounds access at index -1.

## Decoration

The compositor draws window frames:

- **Shadow** — an alpha ring `{58, 46, 34, 24, 15, 8}` with a +3 px downward
  offset.
- **Edge** — `KWM_EDGE_COLOR` at `KWM_EDGE_ALPHA`.
- **Title bar** — rounded top corners; focused color `KWM_TITLEBAR_COLOR`
  (`0x2D2D2D`), inactive `KWM_TITLEBAR_INACT` (`0x252526`); title text in
  `KWM_TITLE_FG` (`0xD4D4D4`).
- **Close button** — `KWM_CLOSE_BTN_W` wide; hover background
  `KWM_CLOSE_HOVER_BG` (`0xE81123`), glyph `KWM_CLOSE_HOVER_FG`.

`frame_dirty_area(x, y, w, h)` marks the shadow-inclusive dirty rectangle
(`x-6, y-6, w+12, h+12+3`); the `+3` accounts for the shadow's downward offset.

## Locking

There is one global `kwm_lock`. Critical sections are deliberately narrow:
copying and scanning happen **outside** the lock, with commit-time
revalidation. The compositor only try-locks `kwm_lock` (deferring on
contention), while input functions use a plain lock. This was narrowed to
prevent input stalls during application launch.

## ABI

`kwm_window_info_t` (`include/kwm_abi.h`) is frozen; `sizeof` is 68 bytes:

| Field | Offset |
| --- | --- |
| `win_id` | 0 |
| `active` | 4 |
| `focused` | 5 |
| `x` | 8 |
| `y` | 12 |
| `width` | 16 |
| `height` | 20 |
| `z_index` | 24 |
| `owner_task` | 28 |
| `flags` | 32 |
| `title[32]` | 36 |

`win_id = slot + 1`; `0` means empty. This is the ABI for syscall 61
(`get_windows`). `_Static_assert` checks enforce the offsets.

## Window Syscalls

| # | Name |
| --- | --- |
| 30 | `create_window` |
| 31 | `update_window` |
| 32 | `destroy_window` |
| 58 | `set_cursor` |
| 59 | `create_desktop` |
| 60 | `set_title` |
| 61 | `get_windows` |
| 62 | `activate_window` |
| 63 | `get_screen_size` |
| 66 | `update_window_rect` |
| 67 | `set_window_opaque` |

See [Syscall Table](../reference/syscalls.md).

## Related Documentation

- [Graphics Overview](README.md)
- [Compositor](compositor.md)
- [Display & Primitives](display.md)
- [GUI](../gui/README.md) — the toolkit and desktop on top of KWM
