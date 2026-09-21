# libdesktop — framework desktop KyuzenOS (infrastruktur, bukan kebijakan)

`libdesktop` adalah satu-satunya jembatan antara kernel/KWM dan implementasi
desktop. Ia menyediakan mekanisme (lifecycle, event, surface, query window,
jam); semua tampilan dan perilaku milik `apps/<implementasi>/`.

```text
kernel/KWM/syscall  ──backend──▶  libdesktop (API publik <kyuzen/desktop/...>)
                                        │
                        ┌───────────────┴───────────────┐
                        ▼                               ▼
                apps/desktop/                  apps/test-desktop/
                (Kyuzen Desktop)               (bukti replaceability)
```

## API publik (`include/kyuzen/desktop/`, di-stage ke C++ SDK)

| Header | Isi |
|---|---|
| `geometry.hpp` | `Point/Size/Rect` (+`contains/intersects`), `Color`, `rgb()` — header-only, tanpa dependensi |
| `event.hpp` | `EventType` (None/MouseMove/MouseButton/Key/Window/Quit), `Event`, `EventPoller::poll()` |
| `system.hpp` | `System::uptime_ms/yield/spawn/exit/poll_crash` + `CrashReport` — tanpa nomor syscall |
| `window_manager.hpp` | `WindowInfo{id,title,focused,is_desktop}`, `WindowManager::get_windows/activate` |
| `canvas.hpp` | `Canvas` (PIMPL di atas libgui) + `Damage{None,Partial,Full}` |
| `shell.hpp` | `Shell`: `on_start/on_event/on_poll/render/is_running` |
| `application.hpp` | `Application::run(Shell&)` — pemilik event loop |

Aturan boundary (ditegakkan `tools/desktop-phase8/check-desktop-isolation.sh`,
`make desktop-isolation`):

- Header publik hanya boleh include `<stdint.h>` + `<kyuzen/desktop/...>`.
  Tanpa `userlib.h`/`libgui.h`, tanpa path privat, tanpa simbol ABI mentah.
- Backend (`src/`) boleh memakai wrapper C SDK, tetapi tidak boleh menyebut
  modul implementasi (`Launcher`, `Taskbar`, `DesktopShell`, …).
- `Damage::Partial` itu generik ("area dinamis saja") — kata "taskbar"
  tidak pernah muncul di framework.

## Kontrak implementasi desktop pihak ketiga

1. Bangun sebagai ELF userspace Kyuzen biasa (statis, `_start` via CRT SDK,
   0 undefined, tanpa SSE/x87 — guard link menolak selain itu).
2. Pakai C++ SDK (`kyuzen-c++`; C++17, `-fno-exceptions -fno-rtti`).
3. Pakai `libdesktop` (jangan panggil `sys_get_event`/`sys_kwm_*` langsung;
   wrapper C SDK untuk FS/CRT boleh).
4. Sediakan `main()` (linkage C) yang merakit Shell + Application.
5. Tanpa perubahan kernel, KWM, atau `libdesktop`.

Contoh minimum: `apps/test-desktop/main.cpp` (±100 baris).
Pilih implementasi saat build: `make desktop DESKTOP_APP=my-desktop`
(output selalu mengisi slot boot `build/apps/desktop.elf`).
