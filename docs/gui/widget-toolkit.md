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
wraps the canvas and provides clipping, geometry (`rect`, `rrect`, `surface`,
`outline`, `focus_ring`, `shadow`), and text/icon drawing.

Text goes through **type roles**, not raw coordinates:

```cpp
p.text_role(title, x, y, color, p.theme.type.title);
p.text_ellipsis(name, x, y, max_w, color, p.theme.type.body);  // clips + "..."
```

`text_ellipsis()` is what keeps user text from overflowing its control — use it
for any string whose length is not known at compile time.

### Theme

`Theme` (`core/theme.hpp`) holds the resolved palette **plus** the design
tokens (`Metrics`, `Typography`). Widgets read everything from it; they never
choose colours, radii, spacing, or type sizes themselves.

Themes come from a **mode × accent** config (`ui_theme_config_t`), and the full
palette is derived deterministically:

```c
ui_theme_config_t cfg;
cfg.mode   = UI_THEME_DARK;     // DARK | LIGHT
cfg.accent = UI_ACCENT_PURPLE;  // NEUTRAL|BLUE|PURPLE|GREEN|ORANGE|RED|CUSTOM
ui_window_set_theme_config(win, &cfg);
```

The legacy six-colour `ui_theme_t` path is still supported and renders
pixel-identically. Persistence is `settings.ui`: `"KTH2"` + mode + accent +
custom (10 bytes), `"KTH1"` + six `color_t` (28 bytes), or the old 24-byte
format (read-only).

See the [Design System](../design/gui/libui-design-system.md) for the token
tables and the [Theme System](../design/gui/ui-theme-system.md) for the
derivation rules.

### State

`core/state.hpp` defines the interaction-state model once, so every control
behaves the same way:

```cpp
ui::StateInputs st;
st.hover = hover; st.pressed = pressed;
st.focused = has_focus; st.enabled = enabled;
color_t fill = ui::state_surface(p.theme, st);
color_t bd   = ui::state_border(p.theme, st);
```

Priority is `disabled > pressed > hover`; focus only affects the **border**,
never the fill colour.

### Damage

Widgets mark damage rectangles as they change. The toolkit aggregates them and
updates the KWM window via `update_window_rect` (syscall 66), so the compositor
only repaints changed regions.

## The C ABI

`include/libui.h` declares the public interface. Representative groups:

| Group | Examples |
| --- | --- |
| Window | `ui_window_create`, `ui_window_set_title`, `ui_window_run` |
| Primitives | `ui_button_create`, `ui_label_create`, `ui_slider_create`, `ui_switch_create` |
| Containers | `ui_vbox_create`, `ui_hbox_create`, `ui_grid_create`, `ui_section_create`, `ui_scrollview_create`, `ui_tab_create` |
| Lists | `ui_listview_create`, `ui_listview_add_row` (icon + title + description + chevron) |
| Chrome | `ui_menubar_create`, `ui_toolbar_create`, `ui_statusbar_create` |
| Dialogs | `ui_dialog_show`, `ui_prompt_show`, `ui_window_notify` |
| Theme | `ui_window_set_theme`, `ui_window_set_theme_config`, `ui_settings_save/load` |
| Icons | `ui_button_set_icon`, `ui_icon_size` |
| Images | `ui_image_set_fit`, `ui_image_natural_size` |

The ABI is stable; adding functions is append-only.

## Notable Widgets

| Widget | Notes |
| --- | --- |
| `TextEdit` | The only editor widget; supports undo/redo, selection, find/replace, word wrap. Uses a single `apply_replace()` mutation rule. |
| `ListView` | Rich rows (icon / title / description / trailing / chevron) with dynamic row height. Arrow/Home/End/PgUp/PgDn move **and** select; Enter activates. |
| `Section` | Titled page section: `section` type role + a hairline rule. The building block for settings-style pages — hierarchy from typography, not cards. |
| `Switch` | Immediate-effect setting (track + knob). Distinct from `CheckBox`, which is a form value submitted with a Save button. |
| `Table` / `GridView` | Row/column selection and hit-testing |
| `Menu` | Separator indexing; bounds are recomputed on relayout; the check gutter only appears when an item is checked |
| `Slider` | Clamps to handle width; guards divide-by-zero when width ≤ handle |
| `PromptDialog` | Text input with a shared `INPUT_PAD_X` offset so click/caret align |

## Icons

`theme/icons.hpp` defines one coherent icon set: 1px stroke geometry on a 16px
grid, drawn by `Painter::icon()`. Applications use **semantic names**
(`UI_ICON_SETTINGS`, `UI_ICON_CHEVRON_DOWN`, …) rather than drawing shapes.
Emoji are not used as UI icons.

## Build Notes

- The toolkit has no global constructors (`.init_array` is not run), so it must
  not rely on static initialization order.
- A single `runtime.cpp` provides ODR-unique runtime symbols.

## Current Limitations

- No cross-process clipboard or drag-and-drop (intra-window only).
- Motion tokens exist (`theme/motion.hpp`) but no transition uses them yet —
  deliberately: hover/press must feel instant, and animation without a purpose
  is not added.
- Text rendering in the toolkit is the built-in 8×16 bitmap path (fixed 8px
  advance). Type roles control line height, tracking, and case; per-role pixel
  sizes are honoured only by applications that render through
  `ui_fttext_*` / `libs/text`.
- High-DPI scaling is not implemented; tokens are in pixels.

## Related Documentation

- [GUI Overview](README.md)
- [Design System](../design/gui/libui-design-system.md) — **how to build UI**
- [Redesign Audit & Report](../design/gui/libui-redesign-report.md)
- [XML UI](xml-ui.md)
- [libdesktop](libdesktop.md)
- [Graphics: Display & Primitives](../graphics/display.md) — the color contract
