# Applications

KyuzenOS ships a set of userspace applications and test programs. Application
binaries are ELF files stored under `/apps/` on the KyuzenFS disk, optionally
accompanied by a `<name>.app` manifest.

## Bundled Applications

| Application | Description |
| --- | --- |
| `desktop` | Desktop environment (launcher, taskbar, wallpaper, crash notice) |
| `fileman` | File manager for KyuzenFS |
| `viewer` | Image viewer (PNG gallery, auto-fit, zoom, scrollbars) |
| `terminal` | Terminal emulator (a front end to the shared shell engine) |
| `notepad` | Text editor (undo/redo, find/replace, word wrap) |
| `clock` | Real-time clock widget |
| `calc` | Calculator |
| `taskmgr` | Task / system monitor |
| `settings` | System settings |
| `gallery` | Image gallery with a thumbnail LRU cache |
| `imageview` | Image viewer |
| `browser` | Web browser (HTML/CSS subset) |
| `procinfo` | Process information viewer |
| `widget_demo` | Widget toolkit showcase |
| `xml_demo` | XML declarative UI demo |
| `fontdemo` | Font rendering demo |
| `badptr` | Ring-3 isolation self-test |

CLI utilities: `echo`, `cat`.

Test programs (in-OS): `procinfo`, `exit_test`, `kill_test`, `fd_test`,
`pipe_test`, `fork_test`, `badptr` (see [Testing](../development/testing.md)).

## Application Layout

```text
apps/
├── Makefile          # application build rules
├── app.ld            # linker script (single base 0x4000000)
├── calc.c  clock.c  notepad.c  terminal.c  viewer.c
├── widget_demo.c  xml_demo.c  fontdemo.c
├── filemanager/      # C++ application
├── gallery/          # C++
├── imageview/        # C++
├── settings/         # C++
├── taskmgr/          # C++
└── browser/          # C++ (engine + app)
```

Kernel-context programs (compiled into the kernel, Ring 0) live in `system/`:
`shell.c`, `shell_core.c`, `login.c`, `zen.c`.

## Manifests

Each application may have a `<name>.app` manifest in `manifests/`. The format
is `key=value`:

| Key | Meaning |
| --- | --- |
| `name` | Display name |
| `color` | Icon background color |
| `icon` | Custom icon file |
| `hidden` | Hide from the launcher |
| `wallpaper` | Desktop wallpaper (desktop only) |

The desktop launcher scans `/apps` for `*.elf` and `*.app` files.

## Name Resolution

The ELF loader resolves a bare application name (no `/`) to `/apps/<name>`.
Absolute paths are used as-is. So `clock` and `start calc` resolve to
`/apps/clock.elf` and `/apps/calc.elf`.

## Building Applications

```sh
make apps        # build the SDK and all C, C++, and Rust applications
make desktop     # build only desktop.elf
make rust-apps   # build the Rust applications
```

Output lands in `build/apps/*.elf`. The build instructions are in
[Building](../development/building.md).

## Adding a New Application

A C application:

1. Create `apps/myapp.c` with `int main(int argc, char **argv)`.
2. Add it to `apps/Makefile` (`APP_ELFS` or the relevant list).
3. Optionally add `manifests/myapp.app`.
4. Build with `make apps`; the ELF lands in `build/apps/myapp.elf`.
5. It is installed to `/apps/myapp.elf` at boot (add a Limine module entry).

A C++ application follows the same steps using the C++ SDK (`kyuzen-c++`); see
[C++ SDK](../libraries/cpp-sdk.md).

For a GUI application, use the widget toolkit ([GUI](../gui/README.md)) or the
`libdesktop` framework ([libdesktop](../gui/libdesktop.md)).

## Application Model

- Applications are independent Ring-3 tasks. `start <app>` spawns one
  concurrently; typing the name execs in place.
- Each application owns its KWM windows; windows are torn down when the task
  exits.
- Applications receive events (keyboard, mouse, window, quit) through their
  event queue (syscall 29).

## Related Documentation

- [Userspace Model](overview.md)
- [Shell & CLI](shell.md)
- [Building](../development/building.md)
- [C SDK](../libraries/c-sdk.md), [C++ SDK](../libraries/cpp-sdk.md)
- [GUI](../gui/README.md)
