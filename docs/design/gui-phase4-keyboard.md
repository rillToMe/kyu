# Desain: Phase 4 — Keyboard Driver → Key Event (key up + modifier)

> **Status**: SELESAI (2026-07-27) — build clean (clang64); verifikasi
> end-to-end di QEMU (sendkey + screendump pada prompt login).
> **Konteks**: Checklist terakhir Phase 4. Sebelumnya keyboard hanya
> mengirim `EVENT_KEY_PRESS` ber-ASCII saat tombol ditekan — tidak ada
> key up, dan satu-satunya modifier yang dilacak adalah Shift (internal).

## Keputusan ABI (additive — app lama tidak terpengaruh)

Layout event keyboard (param1/param2/param3):

| Event | P1 | P2 | P3 |
|---|---|---|---|
| `EVENT_KEY_PRESS` (1) | ASCII (shift/caps diterapkan; 0 = non-printable) | bitmask `KEY_MOD_*` | scancode |
| `EVENT_KEY_RELEASE` (5) | ASCII dasar (tanpa shift/caps) | bitmask `KEY_MOD_*` (setelah release diproses) | scancode |

- Makna `EVENT_KEY_PRESS` P1 **tidak berubah** — app lama (notepad, calc,
  viewer, fileman, clock, libgui) yang hanya membaca P1 jalan terus.
- P3 = scancode set-1; **bit `0x100` menyala = extended (prefix E0)** —
  Ctrl/Alt kanan dan arrow keys tidak tertukar dengan padanan
  non-extended-nya (numpad).
- Pairing press↔release lewat **P3**, bukan P1. P1 release memakai ASCII
  dasar supaya stabil terhadap urutan pelepasan modifier (press 'A' bisa
  saja dilepas setelah Shift lebih dulu dilepas).
- Modifier: `KEY_MOD_SHIFT/CTRL/ALT/CAPS` = `0x01/0x02/0x04/0x08`,
  didefinisikan di `userlib.h` (mirror di `drivers/keyboard.c`).
- `EVENT_KEY_RELEASE` = 5 ditambahkan di `kernel/event.c` + `userlib.h`
  (pola yang sama seperti `EVENT_SCROLL` kemarin).

## Perilaku baru di driver (`drivers/keyboard.c`)

- **CapsLock**: toggle SEKALI per tekan (menahan ≠ toggle ulang); hanya
  memengaruhi huruf, XOR dengan Shift (caps+shift = huruf kecil).
- **Modifier dilacak per sisi** (kiri/kanan, termasuk varian E0 untuk
  Ctrl/Alt kanan) — melepas Shift kiri tidak membatalkan Shift kanan
  yang masih ditahan.
- **Ctrl/Alt + tombol = shortcut, bukan teks**: karakternya tidak masuk
  TTY buffer (shell/login tidak mengetik hurufnya), tapi eventnya tetap
  terkirim lengkap (P1 ASCII, P2 modifier) untuk app GUI.
- **Event untuk SEMUA tombol**: press/release non-printable (F1–F12,
  arrow, Home/End, dll.) terkirim dengan P1=0 — siap dipakai Phase 5
  untuk shortcut Window Manager.
- **Prefix E0 didekode** sebagai state 1-byte; sebelumnya scancode E0
  kebetulan "selamat" karena bit 7-nya menyala (terbaca sebagai release),
  tapi pasangan byte-nya terinterpretasi sebagai tombol non-extended.

## Fix laten: event ikut mati saat TTY buffer penuh

Dulu `push_event` berada **di dalam** cek sisa ruang `kbd_buffer`. Saat
app GUI berjalan, shell tidak membaca TTY buffer — setelah 256 keystroke
buffer penuh dan aliran `EVENT_KEY_PRESS` ke app ikut berhenti total.
Sekarang event push berdiri sendiri di luar `kbd_lock`; TTY buffer hanya
menyimpan teks (drop saat penuh — perilaku lama dipertahankan, isinya
tetap di-flush saat ganti app).

## Verifikasi (QEMU, 2026-07-27)

Metode: boot headless (`-display none`), injeksi `sendkey` via monitor
HMP (TCP), observasi echo prompt **Username** login lewat `screendump`.

| Injeksi | Layar | Kesimpulan |
|---|---|---|
| `r`, `o`, `shift-t` | `roT` | jalur ASCII + shift lama utuh |
| `caps_lock`, `a` | `roTA` | CapsLock bekerja |
| `shift-b` (caps aktif) | `roTAb` | caps XOR shift = huruf kecil |
| `caps_lock`, `c` | `roTAbc` | toggle off bekerja |
| `ctrl-d`, lalu `e` | `roTAbce` (tanpa `d`) | Ctrl = shortcut; tidak lengket setelah dilepas |
| `backspace` | `roTAbc` | jalur kontrol lama utuh |
| `alt_r-x`, lalu `f` | `roTAbcf` (tanpa `x`) | Alt kanan via prefix E0 = modifier; tidak lengket |

"Ctrl/Alt tidak lengket" membuktikan jalur **release memproses modifier**
berjalan benar — bila release rusak, bit modifier akan menempel dan semua
ketikan berikutnya ikut tertahan sebagai shortcut.

## Batasan diketahui

- Routing key event ke window tertentu (focus) = tanggung jawab Phase 5,
  sengaja tidak dibangun di sini (sesuai catatan roadmap).
- LED keyboard belum di-set (CapsLock LED tidak menyala di hardware
  nyata) — command `0xED` bisa ditambahkan bila dibutuhkan.
- Pause (prefix E1) tidak didekode khusus — menghasilkan event phantom
  tak berbahaya (P1=0).
- NumLock belum dilacak; numpad mengikuti tabel apa adanya
  (perilaku lama dipertahankan).

## File yang berubah

| File | Isi |
|---|---|
| `drivers/keyboard.c` | handler baru: key up, modifier per-sisi (L/R + E0), CapsLock, prefix E0, decouple event ↔ TTY buffer |
| `include/userlib.h` | `EVENT_KEY_RELEASE` (5), `KEY_MOD_*`, dokumentasi layout P1–P3 |
| `kernel/event.c` | mirror `EVENT_KEY_RELEASE` |
| `roadmap/GUI_ROADMAP.md` | checklist Phase 4 ditandai selesai |
