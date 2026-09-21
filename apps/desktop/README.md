# Kyuzen Desktop (`apps/desktop/`) — implementasi default di atas libdesktop

File ini implementasi, BUKAN framework. Kebijakan (wallpaper, launcher,
taskbar, notifikasi) tinggal di sini; mekanisme (loop, event, surface)
milik `libdesktop`. Desktop lain (`apps/test-desktop/`) mengganti direktori
ini tanpa menyentuh kernel/framework.

## Modul

| File | Milik |
|---|---|
| `main.cpp` | komposisi saja: `Application` + `DesktopShell` + `run()` |
| `desktop_shell.*` | `DesktopShell : Shell` — wallpaper, kursor, jadwal re-scan 5 dtk, koordinasi render Full (semuanya) vs Partial (taskbar saja) |
| `launcher.*` | scan `/apps` → `*.elf` + manifest `<base>.app` (`name/color/hidden`), grid ikon, klik → `spawn` |
| `taskbar.*` | poll `WindowManager`, change-detection, tombol → `activate` |
| `crash_notice.*` | probe `poll_crash` sekali saat startup, timeout `NOTIF_MS`, klik kartu → File Manager |
| `theme.hpp` | metrik + palet (privat, tidak di-stage) |
| `sys_abi.hpp` | satu-satunya wrap `userlib.h` sisi implementasi |

State eksplisit di objek (`Launcher`, `Taskbar`, `CrashNotice`,
`DesktopShell`) — tidak ada global mutable.

## Perilaku yang dipertahankan dari desktop generasi C

- Launcher tanpa hardcode (`name/color/hidden` dari manifest).
- Taskbar: discovery + fokus + judul + aktivasi.
- Crash notice sekali-tampil (klik luar menutup tanpa mengonsumsi klik).
- Damage: Full awal/berubah-grid/berubah-notice; Partial saat klik/fokus;
  gerak pointer murni tanpa repaint.

## Build

`make desktop` (Lewat `apps/` → ISO juga). Ganti implementasi:
`make desktop DESKTOP_APP=test-desktop`. Guard: `make desktop-isolation`,
host test: `make test-desktop`.
