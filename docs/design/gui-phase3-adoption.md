# Desain: Adopsi Phase 3 — DisplayBuffer & Viewport Jadi Pipeline Nyata

> **Status**: SELESAI (2026-07-27) — build clean (clang64); verifikasi visual
> oleh user.
> **Konteks**: Audit menemukan 3A (DisplayBuffer) & 3C (Viewport) berstatus
> "API-only dead code" — checklist tercentang tapi pipeline nyata memakai
> array mentah. Keputusan user: adopsi penuh (opsi A).

## Perubahan

### 1. 3A — DisplayBuffer jadi mesin layar (`fb.c`, `display.h/c`)

- `base_canvas` / `backbuffer` / `fb_ptr` dibungkus **DisplayBuffer statis**
  (borrow, tanpa kmalloc — heap belum hidup saat framebuffer ditangkap).
  Akses via `gfx_screen_buffer()` / `gfx_back_buffer()` / `gfx_fb_buffer()`.
- Semua primitive `draw_pixel/rect/char/image/string` menggambar lewat
  DisplayBuffer layar (stride-aware) — `draw_rect` memakai
  `display_buffer_fill_rect` (clipping benar, loop per baris).
- `DisplayBuffer` dapat field baru `dirty` (opsional, NULL = tanpa tracking):
  `write_pixel`/`fill_rect` auto-mark. TANPA lock — hanya untuk buffer yang
  disentuh satu konteks. Buffer layar sengaja `dirty = NULL`: marking layar
  tetap lewat `screen_mark_dirty` (jalur ber-lock compositor).

### 2. 3B — Kontrak auto-mark ditepati

Semua primitive menandai dirty **sendiri** (dulu `draw_pixel` tidak —
footgun "lupa mark" hilang). Loop internal memakai `screen_put()` tanpa mark
lalu menandai SATU rect di akhir (mark per-pixel = badai spinlock).
Mark manual redundan di syscall 22 dihapus.

### 3. 3C — Viewport jadi mesin blit compositor

- `viewport_render` dioptimalkan: rentang baris/kolom di-clip SEKALI per
  panggilan, inner loop `memcpy` per baris (dulu bounds-check per pixel).
- Blit compositor (base→back per dirty rect, back→fb present) =
  `viewport_render` dengan `scroll = posisi rect` (`blit_rect_db`).
- Komposit window TETAP loop khusus — butuh alpha-mask per pixel (byte
  alpha = mask transparansi), di luar kontrak Viewport ("cuma memotong &
  menggeser").

### 4. Surface KWM = DisplayBuffer (fondasi Phase 5)

`kwm_window_t.canvas`: `uint32_t*` → **`DisplayBuffer*`** via
`display_buffer_create/destroy` (owned, stride == width). Compositor membaca
`canvas->pixels/stride`. Kontrak "window = Surface = instance DisplayBuffer"
dari roadmap Phase 5 kini nyata.

### 5. Scroll history bisa dipakai (3D + sepotong Phase 4)

- `drivers/mouse.c`: IntelliMouse "magic knock" (sample rate 200→100→80,
  device ID 3) → paket 4 byte, byte[3] = Z.
- Routing wheel (di luar `mouse_state_lock` — render scrollback berat):
  - tidak ada window aktif (`kwm_has_active_windows()`) →
    `tty_scroll_view(-z * 3)` (wheel atas = masuk riwayat);
  - ada window → `push_event(EVENT_SCROLL=4, z, x, y)` ke app.
- `EVENT_SCROLL` ditambah di `event.c` + `userlib.h` (additive, app lama
  tidak terpengaruh).

### 6. Fix bug shell: perintah salah menghapus layar + history

`apps/shell.c` meng-`clear_screen()` SEBELUM `sys_exec` tanpa cek file —
perintah tak dikenal ikut menghapus layar, dan `tty_clear` me-reset ring
history (scrollback hilang). Fix: `sys_file_exists(elf_filename)` dulu;
tidak ada → cetak "Perintah tidak dikenali" tanpa clear; ada tapi gagal
dimuat → "Gagal memuat: <nama>".

## Batasan diketahui

- `tty_clear` masih me-reset ring history — app yang memanggil
  `clear_screen` (mis. badptr) menghapus scrollback. Perilaku "clear = reset
  history" dipertahankan; pisahkan "clear layar" vs "clear history" bila
  nanti dibutuhkan.
- `tty_scroll_view` dari IRQ mouse me-render satu layar penuh di konteks
  interrupt — bisa beberapa ms per notch; dapat dipindah ke deferred work
  bila terasa.
- Arah Z PS/2 (+1 = wheel bawah) — bila terbalik di hardware nyata, balik
  tanda di satu titik routing `mouse.c`.

## File yang berubah

| File | Isi |
|---|---|
| `include/display.h` | field `dirty`, komentar kontrak auto-mark |
| `kernel/display.c` | auto-mark write_pixel/fill_rect; `viewport_render` optimal (clip + memcpy) |
| `kernel/gfx/fb.c` | DisplayBuffer statis layar/back/fb + accessor; primitive via db |
| `include/gfx.h` | accessor `gfx_*_buffer()`, catatan kontrak dirty |
| `kernel/gfx/compositor.c` | blit via `viewport_render`; komposit baca canvas db |
| `kernel/gfx/kwm_internal.h`, `kwm.c` | canvas = `DisplayBuffer*`; `kwm_has_active_windows()` |
| `include/kwm.h` | deklarasi helper baru |
| `drivers/mouse.c` | IntelliMouse wheel + routing scroll |
| `kernel/event.c`, `include/userlib.h` | `EVENT_SCROLL` (4) |
| `kernel/syscall.c` | hapus mark redundan syscall 22 |
| `apps/shell.c` | cek file exists sebelum clear+exec |
