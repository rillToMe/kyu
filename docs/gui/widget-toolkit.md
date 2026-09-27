# Widget Toolkit (libui)

The KyuzenOS widget toolkit (`libs/gui/widget/`) is a C++ library with a stable
**C ABI** (`include/libui.h`) used by both C and C++ applications. It provides
widgets, painting, layout, and theming on top of a KWM window.

## Architecture

The toolkit is organized into layers with a strict one-directional dependency
rule: a layer may only use layers below it.

| Layer | Directory | Contents |
| --- | --- | --- |
| `core` | `core/` | Base `Widget`, `Painter`, `Theme`, event types |
| `runtime` | `runtime/` | Runtime support (memory, string helpers) |
| `primitives` | `primitives/` | Labels, buttons, sliders, checkboxes, etc. |
| `editor` | `editor/` | Text editing (`TextEdit`) |
| `layout` | `layout/` | Box/stack layout, sizing |
| `containers` | `containers/` | Panels, scroll views, tabs, lists, grids |
| `chrome` | `chrome/` | Menu bar, toolbar, status bar |
| `dialog` | `dialog/` | Modal dialogs, file dialogs, prompt dialogs |
| `window` | `window/` | The top-level window widget |
| `services` | `services/` | Shared services (fonts, images, damage) |
| `xml` | `xml/` | XML declarative UI |
| `abi` | `abi/libui_abi.cpp` | The `ui_*` C ABI implementation |

The C ABI (`include/libui.h`, 175+ `ui_*` functions) is the contract for
applications. C++ code uses the same ABI through thin wrappers.

## Key Concepts

### Widget

The base `Widget` class (`core/widget.hpp`) provides:

- Position and size, parent/child relationships
- A `paint(Painter&)` method
- Event handling (`on_click`, `on_pos_click`, `on_drop`, key events)
- Focus and hover state
- A visibility flag and a damage (`dirty`) region

### Painter

`Painter` (`core/painter.hpp`) is the drawing interface handed to widgets. It
wraps the canvas and provides `fill_rect`, `draw_text`, `image`, and clipping.

### Theme

`Theme` (`core/theme.hpp`) holds the color palette and metrics. Themes are
loaded and applied globally; the `ui_theme_t` structure is six `color_t`
values. Theme changes are staged as `"KTH1"` plus six `color_t` (28 bytes); the
legacy 24-byte format is still read.

### Damage

Widgets mark damage rectangles as they change. The toolkit aggregates them and
updates the KWM window via `update_window_rect` (syscall 66), so the compositor
only repaints changed regions.

## The C ABI

`include/libui.h` declares the public interface. Representative groups:

| Group | Examples |
| --- | --- |
| Window | `ui_window_create`, `ui_window_set_title`, `ui_window_run` |
| Primitives | `ui_button_create`, `ui_label_create`, `ui_slider_create` |
| Containers | `ui_panel_create`, `ui_scrollview_create`, `ui_tabs_create` |
| Chrome | `ui_menubar_create`, `ui_statusbar_create` |
| Dialogs | `ui_dialog_message`, `ui_filedialog_open`, `ui_promptdialog` |
| Theme | `ui_theme_set`, `ui_theme_get` |
| Images | `ui_image_set_fit`, `ui_image_natural_size` |

The ABI is stable; adding functions is append-only.

## Notable Widgets

| Widget | Notes |
| --- | --- |
| `TextEdit` | The only editor widget; supports undo/redo, selection, find/replace, word wrap. Uses a single `apply_replace()` mutation rule. |
| Table / GridView | Row/column selection and hit-testing |
| Menu | Separator indexing; bounds are recomputed on relayout |
| Slider | Clamps to handle width; guards divide-by-zero when width ≤ handle |
| PromptDialog | Text input with a shared `INPUT_PAD_X` offset so click/caret align |

## Build Notes

- The toolkit has no global constructors (`.init_array` is not run), so it must
  not rely on static initialization order.
- A single `runtime.cpp` provides ODR-unique runtime symbols.

## Current Limitations

- No clipboard or cross-process drag-and-drop.
- No animation framework.
- Fonts: the toolkit can use bitmap text or the FreeType-backed text library;
  see [Shared Libraries](../libraries/shared.md).

## Related Documentation

- [GUI Overview](README.md)
- [XML UI](xml-ui.md)
- [libdesktop](libdesktop.md)
- [Graphics: Display & Primitives](../graphics/display.md) — the color contract
