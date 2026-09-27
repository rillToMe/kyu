# Desain: Phase 10 — Desktop Environment (desktop shell + Terminal & Settings + port semua app ke libui)

> **Status**: SELESAI (2026-08-06) — build clean (`make boot_image.iso` 0 error);
> verifikasi runtime manual oleh user (QEMU): login → desktop (wallpaper + 8
> ikon + taskbar), klik ikon spawn app, taskbar klik → bring-to-front, Setelan
> ganti tema + Simpan/Muat, Terminal command loop, Kalkulator berhitung,
> Explorer buka/refresh, regresi `start widget_demo`.
> **Konteks**: Phase 9 memberi Desktop Services (clipboard/dialog/notif/dnd/
> cursor/shortcut/settings) di toolkit `libui`. Phase 10 adalah **Desktop
> Environment** sesuai roadmap v1 (Explorer, Terminal, Calculator, Settings,
> Image Viewer, Text Editor): shell desktop GUI yang menggantikan CLI sebagai
> wajah utama boot, ditambah 2 app baru (Terminal, Setelan), dan **semua 6 app
> GUI lama diport dari libgui murni ke libui** sehingga punya titlebar berjudul,
> tema, dan widget.

## Arsitektur hasil

```
Boot: kernel → switch_to_user_mode(user_login)
  user_login: auth sukses → sys_spawn("desktop.elf")   // task konkuren
                          → user_shell()               // CLI tetap hidup di background
```

Dua "desktop" hidup berdampingan: **shell CLI Ring-0** (TTY) dan **window
desktop** (`desktop.elf`, task ring-3). Keyboard menuju TTY saat tidak ada
window yang memegang fokus (`kwm_route_keyboard`: fokus -1 → TTY buffer),
jadi shell tetap bisa diakses di bawah desktop. Desktop window menangkap
klik area kosong → tidak ada refokus.

Tiga lapisan baru di kernel/toolkit yang menjadi fondasi:

1. **Window desktop** (kernel) — full-screen, frameless, z=0, no-focus,
   satu-satunya. Compositor loop mulai dari z=0 dan tidak menggambar
   titlebar/close untuknya; mouse route memakai y-offset 0 (tanpa
   `KWM_TITLEBAR_H`); Alt-Tab & click-to-focus melewatinya.
2. **Titlebar berjudul + window list** (kernel syscall 59–63) — titlebar
   sekarang memuat teks (digambar compositor dengan `font8x16`), dan app
   dapat menanyakan daftar window + mengaktivasi window lain (taskbar).
3. **TextEdit widget + tick callback + image scale** (libui) — widget
   multi-baris untuk Terminal/Text Editor, refresh periodik tanpa event
   (Jam/Task Manager), dan zoom viewer.

## Perubahan file

| File | Perubahan |
|---|---|
| `include/task.h` | `MAX_TASKS` 8 → **16** (desktop + shell + ≥6 app konkuren). |
| `kernel/gfx/kwm_internal.h` | `kwm_window_t` + `uint32_t flags` (desktop) + `char title[32]`. `KWM_WIN_DESKTOP 0x1`. |
| `kernel/gfx/kwm.c` | `kwm_create_desktop()`, `kwm_set_title()`, `kwm_get_windows()`, `kwm_activate_window()`; mouse/fokus/Alt-Tab skip desktop; `kwm_process_mouse` & `kwm_route_mouse` & `kwm_cycle_focus_locked` paham `KWM_WIN_DESKTOP`. |
| `kernel/gfx/compositor.c` | Composite loop `for (z = 0; ...)`; `cty = win_y + (flags&DESKTOP ? 0 : KWM_TITLEBAR_H)`; blok titlebar hanya non-desktop; `titlebar_text()` (font8x16) clamp sebelum tombol close. |
| `kernel/syscall.c` | Syscall **59–63** (lihat bawah). |
| `include/gfx.h` | Deklarasi API KWM baru. |
| `libs/core/userlib.c` + `include/userlib.h` | Wrapper syscall 59–63 + `kwm_window_info_t` (layout = kernel). |
| `libs/core/libgui.c` + `include/libgui.h` | `gui_create_desktop()`, `gui_set_window_title()`. |
| `libs/gui/widget/` + `include/libui.h` | **TextEdit widget**, `ui_window_set_title`, `ui_window_set_tick`, `ui_image_set_scale`, wrapper extern "C". |
| `system/login.c` | Setelah auth sukses → `sys_spawn("desktop.elf")` lalu `user_shell()`. |
| `system/desktop/` (BARU, libgui) | Wallpaper + launcher ikon + taskbar. |
| `apps/terminal.c` (BARU, libui) | Output TextEdit readonly + input TextBox + command loop. |
| `apps/settings.c` (BARU, libui) | Preset tema + Simpan/Muat + info sistem. |
| `apps/fileman.c`, `viewer.c`, `calc.c`, `notepad.c`, `clock.c`, `taskmgr.c` | Port libgui → libui (title + widget + run loop). |
| `apps/Makefile` | ELF `desktop`, `terminal`, `settings`; ported app link `libui.o+png.o`; `desktop.elf` cukup `libgui.o`. |
| `kernel/kernel.c` | `serial_init()` kini selalu dipanggil (bukan hanya `HEAP_WATCH_DEBUG`). |
| `roadmap/GUI_ROADMAP.md` | Phase 10 → SELESAI. |

## Kernel — window desktop + title + list (syscall 59–63)

```c
// 59 — window desktop full-screen, frameless, z=0, no-focus (satu-satunya)
int sys_kwm_create_desktop(void);                     // -> win_id / -1
// 60 — judul titlebar + taskbar (validasi owner)
int sys_kwm_set_title(int win_id, const char* title); // -> 0 / -1
// 61 — enum window utk taskbar (pola sys_get_file_list)
typedef struct {
    uint32_t win_id;      // slot+1; 0 = kosong (konvensi event win_id)
    uint8_t  active;
    uint8_t  focused;     // 1 = pemegang fokus keyboard (tint titlebar)
    int32_t  x, y;
    uint32_t width, height, z_index;
    int32_t  owner_task;
    uint32_t flags;
    char     title[32];
} kwm_window_info_t;
int sys_kwm_get_windows(kwm_window_info_t* buf, int max);  // -> jumlah / -1
// 62 — taskbar klik: bring-to-front + fokus (skip desktop)
int sys_kwm_activate_window(int win_id);              // -> 0 / -1
// 63 — ukuran layar utk desktop (libgui alokasi canvas)
int sys_get_screen_size(uint32_t* w, uint32_t* h);    // -> 0 / -1
```

**`kwm_create_desktop`** (kwm.c:157): reject bila desktop sudah aktif (satu
saja); slot z=0 (`next_z_index` tidak disentuh), `x=0,y=0,w=fb_width,h=fb_height`,
`flags=KWM_WIN_DESKTOP`, `title=""`.

**`kwm_process_mouse`**: saat `target_win` punya `KWM_WIN_DESKTOP` → jangan
ubah `focused_win_id`, jangan `kwm_bring_to_front`, jangan logika
titlebar/drag; tetap teruskan event (ikon/taskbar desktop). **`kwm_route_mouse`**:
`out_ly = sy - win_y` (tanpa `+KWM_TITLEBAR_H`) untuk window desktop.
**`kwm_cycle_focus_locked` / `kwm_refocus_locked`**: lewati desktop.

**`kwm_set_title`** (kwm.c:196): salin max 31 byte ke `title`; wajib
`kwm_window_owner(id) == smp_current_task_id()` (task lain tak boleh ganti
judul); `kwm_frame_dirty` → titlebar direpaint. **`kwm_get_windows`** (kwm.c:215):
isi `buf` dengan window aktif (di bawah `kwm_lock`), return count.
**`kwm_activate_window`** (kwm.c:243): validasi aktif & bukan desktop →
`z_index = next_z_index++` (bring-to-front) + `focused_win_id` + frame dirty
lama & baru.

**Compositor** (compositor.c:137): `for (uint32_t z = 0; z <= next_z_index; z++)`
— desktop (z=0) ikut dikomposit, selalu paling bawah. `cty = win_y +
(flags&KWM_WIN_DESKTOP ? 0 : KWM_TITLEBAR_H)`. Blok titlebar/close
(`! (wflags & KWM_WIN_DESKTOP)`) baru digambar untuk window biasa; bila
`title[0]` → `titlebar_text()` menggambar karakter `font8x16` warna putih di
`(win_x+4, win_y+4)`, clamp `max_x` sebelum tombol close.

### Bug `sys_get_screen_size` (syscall 63) — ditemukan saat verifikasi

Implementasi awal menulis **satu array 8 byte** `uint32_t sz[2] = {fb_width,
fb_height}` ke `rbx` dan mengabaikan `rcx`. Wrapper mengirim **dua pointer
terpisah** (`w` di rbx, `h` di rcx) → byte ke-5..8 menimpa variabel stack
tetangga app. Di `settings.c` tetangga `&sw` adalah pointer widget label
layar → pointer rusak (`0x320` = tinggi layar 1280×800) → `ui_label_set_text`
menderef alamat liar → **page fault user**, task mati, window tersisa canvas
awal putih `0xF5F5F5`, tombol X tak diproses lagi. Diperbaiki menjadi dua
`copy_to_user` 4 byte terpisah (kernel/syscall.c, case 63). Ini menutup bom
waktu yang sama di `gui_create_desktop` (yang lolos cuma karena kebetulan
`sw`/`sh` bersebelahan di stack).

## Toolkit — TextEdit + title + tick + image scale

### TextEdit (libs/gui/widget/include/editor/textedit.hpp)

Class `TextEdit : public Widget`. Buffer `char text[8192]` (MAX_TEXT),
`int len, cur, scroll_top` (baris pertama tampak), `bool readonly`. Metrik:
8 px/karakter, 16 px/baris (sama `font8x16`). `draw`: clip ke rect widget,
gambar baris dari `scroll_top`, caret `|` di posisi kursor saat fokus.
`on_click`: hit-test baris/kolom → `cur`. `on_key`: printable → insert di
`cur` (kecuali readonly); `\n` → newline; backspace; arrow kiri/kanan/atas/
bawah; Home/End; PageUp/PageDown. Auto-scroll bila kursor keluar jendela.

```c
ui_widget_t* ui_textedit_create(ui_window_t* win, int w, int h);
void ui_textedit_set_text(ui_widget_t* w, const char* text);
const char* ui_textedit_text(ui_widget_t* w);       // pointer buffer
void ui_textedit_set_readonly(ui_widget_t* w, int ro);
void ui_textedit_append(ui_widget_t* w, const char* text);  // + auto-scroll bawah
void ui_textedit_clear(ui_widget_t* w);
```

### Title / tick / zoom

- `ui_window_set_title(win, title)` → `gui_set_window_title` → syscall 60.
- `ui_window_set_tick(win, cb, data)` — callback periodik tiap iterasi
  event loop (~60/s, via `sys_yield` + timer IRQ). Return 1 = berubah →
  render. Dipakai Jam (refresh 1×/dtk, tolak bila detik sama) dan Task
  Manager (refresh 2×/dtk).
- `ui_image_set_scale(img, percent)` — ubah ukuran target widget Image
  (zoom viewer), isi ulang dari PNG cache (nearest-neighbor).

## Apps

### desktop.c (libgui, renderer murni — bukan pohon widget)

Alasan pilih libgui bukan libui: butuh kontrol penuh event loop; render
HANYA saat berubah (canvas full-screen mahal — jangan per-frame). Loop:
`while (d->is_running)` → event mouse (move/click) + poll window list
tiap 10 iterasi (~10 Hz, bukan callback — tanpa infra notifikasi kernel) →
bila `winlist_changed()` → `render()` + `sys_kwm_update_window` (push full
canvas) → `sys_yield()`.

- **Wallpaper**: fill `0x141A2E` + label "KyuzenOS" (kanan-atas).
- **Launcher**: grid 2×4 sel (92×100, ikon 56px + label), 8 app:
  Explorer `fileman.elf`, Kalkulator `calc.elf`, Image Viewer `viewer.elf`,
  Text Editor `notepad.elf`, Terminal `terminal.elf`, Setelan `settings.elf`,
  Jam `clock.elf`, Task Manager `taskmgr.elf`. Klik sel → `sys_spawn`.
- **Taskbar** (baris bawah 36px, bg `0x0B0E1C` + edge): tiap window
  non-desktop dengan title → tombol (lebar = `len*8+20`); fokus =
  highlight `0x2E4A8E`. Klik → `sys_kwm_activate_window(win_id-1)`.
- Window desktop tidak tampil di taskbar (`flags & KWM_WIN_DESKTOP` skip).
- Klik taskbar vs ikon dibedakan dengan `my >= H - TB_H`.

### terminal.c (libui) — console window

`VBox[ TextEdit(readonly) 240px, TextBox input ]`, title "Terminal".
TextBox Enter → `on_enter`: echo `kyuzen@de> <cmd>`, split arg, dispatch
ke command loop userspace. Perintah: `help`, `clear`, `echo <teks>`, `ls`,
`baca <file>`, `hapus <file>`, `fetch` (OS/arch/CPU/RAM), `sched` (CPU %),
`time` (HH:MM:SS), `start <app>` (sys_spawn), `ping <host>` (rtt), `shutdown`,
`restart`. Output lewat return value syscall — **bukan PTY**
(`ponytail:`: butuh infra kernel PTY + device node; command loop userspace,
program interaktif belum bisa dijalankan).

### settings.c (libui)

Title "Setelan"; ScrollView-VBox (320×350 + 8): 3 preset tema
(Gelap/Terang/Hijau, sama `widget_demo`), Simpan/Muat
(`ui_settings_save/load`, persist `settings.ui` 24 byte ke KyuzenFS), Info
Sistem: RAM `sys_used_ram/total_ram` (MB), CPU `get_cpu_string`, Layar
`sys_get_screen_size`.

### Port 6 app → libui (in-place)

Pola umum: `gui_create_window`+`gui_mainloop`+draw langsung →
`ui_window_create` + `ui_window_set_title` + widget + `ui_window_run`
(blocking; keluar via X titlebar / ESC → `ui_window_destroy` + `sys_exit`).

- **fileman.c → Explorer**: `Table` nama+ukuran (fill via `sys_get_file_list`);
  klik pilih; tombol Buka/Refresh. `.elf` → `sys_spawn`; `.png` → tulis
  `view.tmp` + spawn `viewer.elf`; `.txt` → tulis `edit.tmp` + spawn
  `notepad.elf` (arg antar-app via temp-file, pola lama fileman→edit.tmp).
- **viewer.c → Image Viewer**: daftar `.png` di `ListView` (sidebar kiri),
  gambar di `Image` dalam `ScrollView`, zoom ± toolbar (`ui_image_set_scale`);
  dari Explorer (`view.tmp`) → muat langsung, tutup → kembali ke Explorer.
  *Diperbarui Phase 11* — lihat
  [`gui-phase11-image-viewer.md`](gui-phase11-image-viewer.md): gambar
  di-auto-fit ke area (tanpa geser manual), tombol nyata + menubar +
  statusbar, scrollbar hanya saat zoom.
- **calc.c → Kalkulator**: display `Label` + grid 5×4 `Button` (HBox per
  baris), state machine `+,-,×,÷,%,+/-,C/CE,=` diambil utuh dari versi
  libgui.
- **notepad.c → Text Editor**: menu File Baru/Buka/Simpan; nama file
  `TextBox`, isi `TextEdit`; dari Explorer (`edit.tmp`) → muat file itu.
- **clock.c → Jam**: `Label` besar + tanggal (Zeller day-of-week), refresh
  `ui_window_set_tick` 1×/dtk (render hanya saat detik berubah).
- **taskmgr.c → Task Manager**: bar RAM/CPU/Disk (`ProgressBar` + label) +
  uptime, refresh 2×/dtk via tick; tanpa per-task table (`ponytail:`:
  kernel tak mengekspos daftar task + usage per-task).

## Boot

`system/login.c`: setelah `sys_set_uid` + jeda → `sys_spawn("desktop.elf")`
(fail → fallback natural ke shell CLI), lalu `clear_screen()` +
`user_shell()`. Shell CLI Ring-0 tetap hidup; keyboard saat tak ada window
fokus → TTY (shell tak mengganggu desktop).

## Build

`apps/Makefile`: ELF `desktop` (link `desktop.o+userlib.o+libgui.o`),
`terminal` & `settings` (link `+libui.o+png.o`); 6 ported app
`CFLAGS_APP` → `CFLAGS_LIB` (viewer O2) + link `libui.o+png.o`.
Top-level `make boot_image.iso` = kernel + semua ELF + ISO (desktop.elf,
terminal.elf, settings.elf masuk `cp` ke `iso_root`). Per-app:
`make -C user_apps desktop|terminal|settings|...`.

## Verifikasi

1. `make clean` + `make boot_image.iso` → build clean (kernel + semua apps + ISO).
2. Boot → login → **desktop muncul** (wallpaper `0x141A2E` + 8 ikon + taskbar kosong).
3. Klik ikon Explorer/Setelan/Terminal → window berjudul di titlebar + taskbar.
4. Taskbar menampilkan tiap window; klik → bring-to-front + fokus (highlight).
5. Setelan: ganti tema → Simpan → Muat → tema balik; info RAM/CPU/Layar tampil.
6. Terminal: `ls`, `echo halo`, `fetch`, `time`, `start clock` → output + jam muncul.
7. Kalkulator berhitung; Image Viewer buka .png + zoom ±; Text Editor Buka/Simpan.
8. Klik wallpaper (area kosong) → tak refokus; Alt-Tab hanya siklus app (bukan desktop).
9. Regresi: `start widget_demo`, `start clock`, `start taskmgr`.

## File

- `kernel/gfx/kwm.c`, `kernel/gfx/compositor.c`, `kernel/gfx/kwm_internal.h` — desktop window, title, list, aktivasi.
- `kernel/syscall.c` — syscall 59–63 (termasuk fix 63).
- `include/gfx.h`, `include/kwm.h` — deklarasi API KWM.
- `libs/core/userlib.c`, `include/userlib.h` — wrapper + `kwm_window_info_t`.
- `libs/core/libgui.c`, `include/libgui.h` — `gui_create_desktop`, `gui_set_window_title`.
- `libs/gui/widget/`, `include/libui.h` — TextEdit, set_title, tick, image scale.
- `system/login.c` — spawn desktop setelah auth.
- `system/desktop/`, `terminal.c`, `settings.c` (baru) + 6 port.
- `include/task.h` — `MAX_TASKS` 16.
- `roadmap/GUI_ROADMAP.md` — Phase 10 → SELESAI.

## Belum ada (phase depan / batas sengaja)

- **Terminal bukan PTY** — command loop userspace; program interaktif butuh
  PTY kernel + device node.
- **Tanpa Canvas widget** — tak ada app yang butuh custom-paint;
  `ui_image_set_scale` + ScrollView menutupi viewer.
- **Taskbar polling 10 Hz** — bukan callback create/destroy window (perlu
  infra notifikasi kernel).
- **TextEdit tanpa selection** — edit teks polos; selection/undo fase depan.
- **Desktop satu** — window desktop global tunggal; tak ada multi-monitor.
- **`sys_kwm_update_window` full-buffer** — tak ada partial-update; desktop
  render hemat (hanya saat berubah).
- **Task Manager tanpa per-task** — kernel tak mengekspos daftar task +
  usage per-task (syscall baru bila fase berikutnya butuh).
