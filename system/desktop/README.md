# Kyuzen Desktop (`system/desktop/`) — implementasi default di atas libdesktop

File ini implementasi, BUKAN framework. Kebijakan (wallpaper, launcher,
taskbar, notifikasi) tinggal di sini; mekanisme (loop, event, surface)
milik `libdesktop`. Desktop lain (`tests/target/test-desktop/`) mengganti direktori
ini tanpa menyentuh kernel/framework.

## Modul

| File | Milik |
|---|---|
| `main.cpp` | komposisi saja: `Application` + `DesktopShell` + `run()` |
| `desktop_shell.*` | `DesktopShell : Shell` — wallpaper, kursor, hover/preview, jadwal re-scan 5 dtk, urutan gambar latar→depan, koordinasi Full vs Partial |
| `wallpaper.*` | pilihan `wallpaper=` dari `/system/desktop.app` (fallback bawaan → gradasi), foto PNG di-decode+skala sekali, digambar RLE ke canvas window (region-aware untuk restore Partial) |
| `launcher.*` | scan `/apps` → `*.elf` + manifest `<base>.app` (`name/color/hidden/icon`), grid ikon, klik → `spawn` |
| `app_icons.*` | resolusi ikon TERPUSAT (kustom → `default.png` → kotak warna), cache 48px, `scale_icon` (box + unsharp `media_sharpen_rgba`, kekuatan `ICON_SHARPEN_PCT`), `draw_px` (blend alpha ke canvas window) |
| `taskbar.*` | slot ikon app (fokus/hover), area sistem kanan (jam `HH:MM` + tanggal `DD MM YYYY` numerik via `sys_get_time`), klik → `activate` |
| `app_preview.*` | kartu preview STATIS saat hover slot (ikon + judul + status; bukan thumbnail live — `WindowInfo` tak membawa pixel) |
| `crash_notice.*` | probe `poll_crash` sekali saat startup, timeout `NOTIF_MS`, klik kartu → File Manager |
| `theme.hpp` | metrik + palet + daftar wallpaper (privat, tidak di-stage) |
| `sys_abi.hpp` | satu-satunya wrap `userlib.h` sisi implementasi |

State eksplisit di objek (`Launcher`, `Taskbar`, `CrashNotice`,
`DesktopShell`, `Wallpaper`, `IconCache`, `AppPreview`) — tidak ada global
mutable.

Pelapisan gambar (kontrak lapisan dengan compositor, tanpa ubahan framework):
window desktop **full-screen + opaque** — `gui_create_desktop()` sudah mengisi
canvas-nya dengan latar opaque dan mendeklarasikannya opaque, sehingga
compositor melewatkan base-blit untuk seluruh layar dan menimpa `base_canvas`.
Karena itu gambar ke `base_canvas` (`sys_draw_image`) TIDAK pernah terlihat,
dan SEMUA pixel (wallpaper, ikon, strip taskbar, kartu preview, notifikasi)
digambar ke canvas window lewat `Canvas::fill_rect`/`draw_text`.

Konsekuensi yang perlu diingat:

- libgui tidak punya draw-image → foto wallpaper di-blit sebagai RLE per
  baris (satu `fill_rect` per rentang warna identik) di
  `Wallpaper::draw_photo_into`; ikon (launcher/taskbar/preview) di-blend
  per-pixel via `Canvas::draw_px` (`libs/gui/libdesktop`) agar alpha PNG utuh.
- `png_decode` (libs/media/png.c) mempertahankan alpha ARGB8888 (a=0 transparan
  dilewati, semi di-blend, hasil canvas selalu opaque agar deklarasi opaque
  compositor valid). Aset ikon boleh memakai transparansi.
- Render Partial (strip taskbar + kartu preview) hanya menggambar area itu;
  bekas kartu preview dipulihkan dari wallpaper lewat `draw_bg(region)`,
  bukan render ulang seluruh layar.
- Buffer layar wallpaper (±8 MB @1080p) diambil dari **allocator standar**
  (`new (std::nothrow) uint32_t[...]`), bukan `sys_alloc`: sejak Phase 9.5
  heap user-space tumbuh on-demand (arena tambahan lewat `sys_alloc` #9), jadi
  permintaan 8 MB tidak lagi `NULL` seperti pada insiden BSOD INT 6. Detail:
  `libs/c/libc-port/src/kyuzen_heap.hpp` + `test/libc_heap_test.cpp`
  (`make test-libc-heap`).

## Perilaku yang dipertahankan dari desktop generasi C

- Launcher tanpa hardcode (`name/color/hidden/icon` dari manifest).
- Taskbar: discovery + fokus + aktivasi (judul pindah ke kartu preview).
- Crash notice sekali-tampil (klik luar menutup tanpa mengonsumsi klik).
- Damage: Full awal/berubah-grid/berubah-notice; Partial saat klik/fokus/
  hover/jam-menit/preview; gerak pointer di luar strip tanpa repaint.
- Aset visual: `assets/wallpaper/*.png` (modul limine → akar FS),
  `assets/icons/default.png` (fallback) + `demo.png` (contoh kustom via
  `icon=` di `manifests/widget_demo.app`).

## Build

`make desktop` (Lewat `apps/` → ISO juga). Ganti implementasi:
`make desktop DESKTOP_APP=test-desktop`. Guard: `make desktop-isolation`,
host test: `make test-desktop`.
