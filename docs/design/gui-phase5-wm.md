# Desain: Phase 5 — Window Manager (KWM): fokus, dekorasi, koordinat, Alt-Tab

> **Status**: 5A–5C SELESAI (2026-08-05), 5D Alt-Tab diimplementasikan
> (build clean). Verifikasi runtime end-to-end: 5B/5C via QEMU (sendkey +
> screendump); Alt-Tab menunggu verifikasi interaktif.
> **Konteks**: KWM + compositor hari ini sudah ±90% adalah "Surface Manager
> (buffer & event)" versi kernel. 5C menutup celah struktural terakhir
> (koordinat global bocor) dan memindahkan dekorasi ke milik WM; 5D
> menambahkan shortcut WM pertama (Alt-Tab). Policy masih di ring 0 — nanti
> (pasca-IPC) pindah ke window server ring-3, ABI yang distabilkan di sini
> menjadi protokol IPC.

## Keputusan geometri: canvas = konten murni, frame = + titlebar WM

Window **tidak lagi** berisi titlebar di canvas-nya. Dekorasi adalah milik WM.

```
frame  ┌────────────────────────────┐  (x, y)         ← posisi FRAME
       │  titlebar (KWM_TITLEBAR_H=24)  [X] 0x1F4E8C  ← digambar compositor
       ├────────────────────────────┤
konten │                            │  (x, y+24)      ← canvas app
       │  width × height            │                  (window-local, y=0)
       └────────────────────────────┘
       ←  width  →  (frame lebar = width konten)
```

- `kwm_window_t.x, y` = sudut kiri-atas **frame** (titlebar). `width, height`
  = ukuran **konten** (= ukuran canvas). Frame tinggi = `height + KWM_TITLEBAR_H`.
- `sys_kwm_create_window(w, h)` tetap berarti ukuran **konten** — makna
  parameter tidak berubah, hanya titlebar pindah keluar canvas.
- Hit-test (`kwm_hit_test_locked`), drag/clamp, dan dirty-region memakai
  rect **frame**. `kwm_update_window` menandai rect **konten** saja (titlebar
  tidak berubah saat app redraw).
- `KWM_TITLEBAR_H=24`, `KWM_CLOSE_BTN_W=40` memformalkan hardcode yang dulu
  duplikat antara `kwm_process_mouse` (drag 24px) dan libgui (`GUI_TITLEBAR_H=30`).
  Satu sumber kebenaran sekarang: `kwm_internal.h`.

## Dekorasi digambar compositor (`kernel/gfx/compositor.c`)

`composite_windows_in_rect` per window (z low→high, clip ke dirty rect):

1. **Konten**: blit canvas dengan alpha-mask per pixel (`pixel>>24`), origin
   digeser ke `(x, y + KWM_TITLEBAR_H)`.
2. **Titlebar**: isi solid `KWM_TITLEBAR_COLOR` (fokus, biru `0x1F4E8C`) atau
   `KWM_TITLEBAR_INACT` (tidak fokus, abu `0x3A3A3A`) — dibaca dari
   `focused_win_id` (extern via `kwm_internal.h`).
3. **Tombol close**: rect `width-40 × 24` kanan, merah `0xE53935`, glyph "X"
   dua segmen garis (DDA, dipotong clip) — tanpa font, tanpa deps baru.

Fokus berubah → titlebar fokus lama & baru ditandai dirty (`kwm_frame_dirty`):
click-to-focus, create (window baru = fokus), destroy (refokus ke topmost).

## Koordinat event = window-local konten (sys_get_window_pos dihapus)

`kwm_route_mouse` menerima layar `(sx,sy)`, mengembalikan owner + `out_lx,
out_ly` yang **sudah window-local konten**:

```
lx = sx - win.x
ly = sy - (win.y + KWM_TITLEBAR_H)
```

- `EVENT_MOUSE_MOVE` (P1,P2) dan `EVENT_SCROLL` (P2,P3) membawa koordinat
  lokal. `EVENT_MOUSE_CLICK` tidak membawa koordinat — app memakai posisi dari
  MOVE terakhir.
- App/libgui **tidak lagi** memanggil `sys_get_window_pos` (syscall 40
  dihapus: handler, wrapper `apps/userlib.c`, deklarasi `userlib.h`, dan
  semua pemakaian di-migrasi). `rel_x/rel_y` = P1/P2 event langsung.

## Protokol close: EVENT_WIN_CLOSE (6), bukan destroy paksa

Klik tombol close titlebar → KWM mengirim `EVENT_WIN_CLOSE` ke **owner task**
(`push_event_to` dari IRQ mouse, konsumsi klik — app tidak dapat CLICK biasa):

- App (libgui + semua app) merespons: `is_running = 0` → cleanup → `sys_exit`
  → `kwm_destroy_windows_of` + `task_exit` (spawned) membereskan window & task.
- Bukan destroy paksa — app berhak konfirmasi/menolak nanti (keputusan #8).

## Alt-Tab (5D): shortcut WM pertama

`drivers/keyboard.c` memanggil `kwm_handle_shortcut(mods, released, key_id)`
**sebelum** routing normal (identitas tombol via `key_id` scancode, bukan
ascii — Tab set-1 = `0x0F`). Return 1 = dikonsumsi (tidak di-route ke app,
tidak masuk TTY).

```
Alt+Tab (press)   → kwm_cycle_focus: fokus ke window z berikutnya (wrap),
                    bring-to-front; alt_tab_active = 1
Tab (release)     → ditelan (Alt masih dipegang)
Alt (release)     → akhiri sesi; ditelan
lainnya           → routing normal
```

- `kwm_cycle_focus_locked` (di dalam `kwm_lock`): fokus ke z tertinggi
  berikutnya dari fokus saat ini; wrap ke paling bawah; tanpa fokus → window
  teratas. Bring-to-front via `z = next_z_index++`.
- Mengandalkan modifier tracking Phase 4 (`KEY_MOD_ALT` di P2) + `key_id`
  scancode (`0x0F` = Tab set-1) untuk pairing press↔release yang stabil.
- Tanpa window: Alt+Tab no-op (ditelan) — Tab tidak mengetik `\t` ke shell.

## Fix bonus (Makefile, pre-existing)

`-include $(OBJS:.o=.d)` terletak **sebelum** target `all` → begitu file `.d`
ada, target pertama file `.d` (`arch/x86/gdt.o`) menjadi default goal dan
`make` hanya membangun gdt.o. Fix: `.DEFAULT_GOAL := all` ditaruh di atas
`-include`.

## Verifikasi

- Build clean: kernel (`myos.bin`) + semua `user_apps` (clang64).
- 5C visual (QEMU screendump): window render dengan titlebar biru fokus +
  tombol close merah, konten di bawahnya.
- Alt-Tab: menunggu verifikasi interaktif (2 window → Alt+Tab → fokus pindah +
  bring-to-front + tint berubah; keyboard mengikuti fokus baru).
