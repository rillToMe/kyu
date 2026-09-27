# libdesktop

`libdesktop` (`libs/gui/libdesktop/`) is the C++ framework that sits between the
kernel/KWM and a desktop implementation. It provides **mechanism**
(lifecycle, events, surfaces, window queries, clock); the appearance and
behavior belong to the implementation in `apps/` or `system/`.

```text
kernel / KWM / syscalls
        │  backend
        ▼
libdesktop (public API <kyuzen/desktop/...>)
        │
        ├───────────────┬───────────────┐
        ▼                               ▼
system/desktop/                  tests/target/test-desktop/
(Kyuzen Desktop)                 (replaceability proof)
```

## Public API

Headers live in `libs/gui/libdesktop/include/kyuzen/desktop/` and are staged
into the C++ SDK.

| Header | Contents |
| --- | --- |
| `geometry.hpp` | `Point`, `Size`, `Rect` (with `contains`/`intersects`), `Color`, `rgb()` — header-only, no dependencies |
| `event.hpp` | `EventType` (None/MouseMove/MouseButton/Key/Window/Quit), `Event`, `EventPoller::poll()` |
| `system.hpp` | `System::uptime_ms/yield/spawn/exit/poll_crash` + `CrashReport` — no raw syscall numbers |
| `window_manager.hpp` | `WindowInfo { id, title, focused, is_desktop }`, `WindowManager::get_windows/activate` |
| `canvas.hpp` | `Canvas` (PIMPL over libgui) + `Damage { None, Partial, Full }` |
| `shell.hpp` | `Shell` interface: `on_start/on_event/on_poll/render/is_running` |
| `application.hpp` | `Application::run(Shell&)` — owns the event loop |

## Boundary Rules

These are enforced by `tools/desktop-phase8/check-desktop-isolation.sh`
(`make desktop-isolation`):

- Public headers may include only `<stdint.h>` and `<kyuzen/desktop/...>` — no
  `userlib.h`, no `libgui.h`, no private paths, no raw ABI symbols.
- The backend (`src/`) may use the C SDK wrappers but must not name
  implementation modules (`Launcher`, `Taskbar`, `DesktopShell`, …).
- `Damage::Partial` is generic ("dynamic areas only"); the word "taskbar" never
  appears in the framework.

## Damage Model

`Canvas` reports one of three damage states per frame:

| State | Meaning |
| --- | --- |
| `Damage::None` | Nothing changed |
| `Damage::Partial` | Only dynamic regions changed |
| `Damage::Full` | The whole window changed |

This maps directly onto the KWM's partial-window update and the compositor's
dirty-region system.

## Third-Party Desktop Contract

A desktop implementation:

1. Builds as a normal Kyuzen userspace ELF (static, `_start` via the SDK CRT,
   0 undefined symbols, no SSE/x87 — the link guard rejects otherwise).
2. Uses the C++ SDK (`kyuzen-c++`; C++17, `-fno-exceptions -fno-rtti`).
3. Uses `libdesktop` (never calls `sys_get_event` / `sys_kwm_*` directly; the C
   SDK wrappers for FS/CRT are allowed).
4. Provides a C-linkage `main()` that assembles a `Shell` and `Application`.
5. Requires no changes to the kernel, KWM, or `libdesktop`.

A minimal example is `tests/target/test-desktop/main.cpp` (about 100 lines).
Select an implementation at build time with
`make desktop DESKTOP_APP=my-desktop` (output always fills the boot slot
`build/apps/desktop.elf`).

## Related Documentation

- [GUI Overview](README.md)
- [Desktop Environment](desktop.md)
- [Widget Toolkit](widget-toolkit.md)
- [C++ SDK](../libraries/cpp-sdk.md)
