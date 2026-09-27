# GUI

KyuzenOS provides a layered userspace GUI stack: a C widget toolkit (`libui`),
an XML declarative UI layer, a C++ desktop framework (`libdesktop`), and a
reference desktop environment.

## Overview

| Layer | Location | Language | Purpose |
| --- | --- | --- | --- |
| Widget toolkit | `libs/gui/widget/` | C++ (C ABI) | Widgets, painting, layout, theming |
| XML UI | `libs/gui/widget/{include,src}/xml/` | C++ | Declarative UI from XML |
| Desktop framework | `libs/gui/libdesktop/` | C++ | Application lifecycle, events, canvas |
| Desktop environment | `system/desktop/` | C++ | Launcher, taskbar, wallpaper, notifications |

| Document | Description |
| --- | --- |
| [Widget Toolkit](widget-toolkit.md) | Layered architecture, widgets, the `ui_*` C ABI |
| [XML UI](xml-ui.md) | Declarative UI, the inflater, binding |
| [libdesktop](libdesktop.md) | The framework API and replaceability contract |
| [Desktop Environment](desktop.md) | The default desktop implementation |

## Layering

```text
Applications (C++ / C)
    │
    ├── libdesktop (Application, Shell, Canvas, Event, System)
    │       │
    │       ▼
    ├── widget toolkit (libui)  ──►  KWM syscalls (30–32, 60–63, 66, 67)
    │       │
    │       ▼
    └── userlib wrappers (syscalls)
```

The widget toolkit is a C++ library exposing a stable **C ABI** (`ui_*`
functions declared in `include/libui.h`) so that C applications can use it
directly. Internally it is organized into layers with a strict dependency
direction.

## Window Integration

Widgets do not talk to the KWM directly. The toolkit owns a KWM window and a
canvas, draws into the canvas, and updates the window through the KWM syscalls.
Input arrives as events from the task's event queue (syscall 29).

The desktop window is created with `gui_create_desktop()` as a full-screen
opaque window; the compositor then skips the base blit for the whole screen.

## Event Flow

```text
PS/2 driver (IRQ)
    └─→ KWM focus → kwm_route_keyboard/mouse
            └─→ target task event queue
                    └─→ sys_get_event (29)
                            └─→ toolkit dispatches to the focused widget
```

Because the focused window owns the keyboard, typed input goes to the focused
application, not to the console shell.

## Current Limitations

- Single-window-per-application model; a widget toolkit window maps to one KWM
  window.
- No clipboard, drag-and-drop across processes, or accessibility layer.
- The XML UI supports a fixed element set (see [XML UI](xml-ui.md)).
- No GPU-accelerated widget rendering; painting is into a CPU canvas that the
  compositor presents.

## Related Documentation

- [Widget Toolkit](widget-toolkit.md)
- [XML UI](xml-ui.md)
- [libdesktop](libdesktop.md)
- [Desktop Environment](desktop.md)
- [Graphics: Window Manager](../graphics/window-manager.md)
