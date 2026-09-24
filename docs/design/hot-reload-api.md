# Hot Reload API (generik)

Satu syscall untuk semua reload runtime config. Aturan: fitur baru menambah
TARGET + owner handler, BUKAN syscall baru.

## ABI

```c
int sys_hot_reload(uint32_t target, uint32_t flags);  // syscall 85 -> 0 / -1
```

- `target` = `enum kz_hot_reload_target` (`include/kwm_abi.h`, satu definisi
  kanonis untuk kernel + userland):
  - `KZ_HOT_RELOAD_WALLPAPER = 1` — owner: Desktop (`/wallpaper.ui`)
  - `KZ_HOT_RELOAD_FONT = 2` — owner: Desktop/launcher (`/font.ui`)
  - `KZ_HOT_RELOAD_MAX = 2` — batas validasi kernel
- `flags` dicadangkan, harus 0 (non-nol = -1).
- Target tak dikenal / flags non-nol / owner tak ada = -1. Tanpa pointer user.

Legacy: syscall 84 `sys_wallpaper_reload()` = alias
`HOT_RELOAD(WALLPAPER)`. Kode baru memakai 85.

## Flow

```text
Settings (persist config dulu)
  ↓ sys_hot_reload(target, 0) — int 0x80, RAX=85, RBX=target, RCX=flags
kernel sys_kwm_handle: validasi → kwm_desktop_owner() → push_event_to(EVENT_HOT_RELOAD, P1=target)
  ↓ syscall 29 (pop antrean per-task)
Desktop EventPoller → Event::HotReload{hot_target}
  ↓ DesktopShell::on_event → reloadWallpaper() / reloadFont()
swap aman (gagal = lama tetap) → Damage → compositor/GHAL existing
```

Kernel tidak decode gambar/font, tidak sentuh compositor/framebuffer.

## Ownership

| Target | Owner | Reload |
|---|---|---|
| WALLPAPER | `DesktopShell::reloadWallpaper()` | `Wallpaper::poll()` (baca `/wallpaper.ui` → manifest), swap hanya bila decode sukses |
| FONT | `DesktopShell::reloadFont()` → `Launcher::ui_font_poll()` | baca `/font.ui`, swap hanya bila face valid |

Event beruntun (A→B→C) diproses satu per satu; tiap handler membaca config
TERKINI sehingga hasil akhir = yang terakhir. Queue penuh = overwrite tertua
(semantik antrean existing — reload selalu re-read, jadi aman).

Polling font tiap rescan 5 dtk DIPERTAHANKAN sebagai jaring pengaman untuk
perubahan di luar API (edit file manual); jalur utama = event seketika.
Polling wallpaper sudah dihapus sebelumnya (event-only).

## Ditunda (tanpa consumer global — jangan tambah target tanpa owner)

- THEME: tema hari ini per-window (`ui_window_set_theme` + `settings.ui`);
  tidak ada broadcast ke semua task, jadi tidak ada target THEME. Settings
  Appearance persist-only. Target THEME baru ditambah bila ada owner global
  + semantik event yang jelas.

## Menambah target baru

1. Tambah nilai enum + naikkan `KZ_HOT_RELOAD_MAX` (`kwm_abi.h` saja).
2. Assign owner; tambah cabang di `DesktopShell::on_event` (atau owner lain).
3. Pakai persistence + invalidation/present existing. Tanpa syscall baru.
