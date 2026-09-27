# Desain: Phase 9 — Desktop Services (Clipboard, Dialog, Notification, Drag & Drop, Cursor, Shortcut, Settings)

> **Status**: SELESAI (2026-08-06) — build clean; verifikasi runtime manual oleh
> user (QEMU): `start widget_demo` → kursor I-beam/hand per-widget, Ctrl+N
> shortcut → dialog, dialog Ya/Tidak/Batal (ESC), toast auto-expire 3 dtk +
> klik dismiss, clipboard Ctrl+C/V/X + menu Salin/Tempel, drag chip → drop zone,
> tema ganti → Simpan/Muat, regresi Phase 8 utuh.
> **Konteks**: Phase 8 memberi Advanced Widgets (ListView/Table/TreeView/
> ScrollView/Menu/MenuBar/Tab/Toolbar). Phase 9 menambahkan **Desktop Services**
> sesuai roadmap: Clipboard, Dialog, Notification, Drag & Drop, Cursor,
> Shortcut, Settings. Semua kerjaan userspace toolkit (`libs/gui/widget/` +
> `include/libui.h`) — kecuali **Cursor**, yang butuh satu syscall kernel kecil
> (compositor pemilik satu-satunya kursor mouse).

## Tujuh layanan, tiga pola baru

1. **Keyboard jadi sumber shortcut & clipboard (Clipboard, Shortcut)**.
   `EVENT_KEY_PRESS` P2 sudah membawa bitmask `KEY_MOD_*` sejak Phase 4 —
   `Window::run` sebelumnya meneruskannya ke `Widget::on_key` dan semua widget
   membuangnya. Phase 9 memakainya: registry shortcut dicek sebelum dispatch ke
   widget fokus; TextBox memakai Ctrl+C/X/V sebagai clipboard.
2. **State overlay modal & temporer (Dialog, Notification)**. Dialog adalah
   overlay modal (klik/ketik di luar diabaikan); Notification adalah toast yang
   auto-expire. Keduanya **per-window canvas overlay**, bukan window kernel
   terpisah — konsisten dgn popup menu Phase 8, tinggal ditaruh paling atas di
   `render()` dan dicegat di `run()`.
3. **Mouse jadi drag & drop (Drag & Drop, Cursor)**. DnD memakai kontrak klik
   yang sudah ada (left-down → left-up) untuk memulai drag ghost dan menge-drop
   ke target. Cursor butuh syscall baru karena compositor kernel yang
   menggambar kursor (tanpa API override).

## Perubahan file

| File | Perubahan |
|---|---|
| `kernel/gfx/compositor.c` | 2 bitmap kursor baru (I-beam, hand) 12×16 + `g_cursor_kind` + `kwm_set_cursor()` (validasi 0..2, mark cursor rect dirty) + selector bitmap di draw loop. |
| `kernel/syscall.c` | Syscall **58** `sys_kwm_set_cursor`: validasi kind, call `kwm_set_cursor`, return 0/-1. |
| `include/gfx.h` | Deklarasi `kwm_set_cursor`. |
| `libs/core/userlib.c` | Wrapper `int sys_kwm_set_cursor(int kind)` (int 0x80, RAX=58). |
| `include/userlib.h` | Deklarasi `sys_kwm_set_cursor`. |
| `include/libui.h` | ~20 deklarasi C ABI baru + 2 tipe callback (`ui_dialog_cb`, `ui_drop_cb`) + enum kursor. |
| `libs/gui/widget/` | Clipboard buffer, field DnD+kursor di `Widget`, Ctrl+C/V/X di TextBox, `Dialog` class + state modal Window, toast notification, DnD state + ghost, shortcut registry, Settings save/load, wrapper extern "C". |
| `apps/widget_demo.c` | Toolbar Dialog/Notif, menu Edit → clipboard, shortcut Ctrl+N, tab Setelan baru. |
| `roadmap/GUI_ROADMAP.md` | Phase 9 → SELESAI. |

## C ABI (include/libui.h)

```c
// --- Clipboard ---
void ui_clipboard_set_text(const char* text);
const char* ui_clipboard_get_text(void);   // pointer buffer internal
void ui_clipboard_clear(void);

// --- Shortcut ---
void ui_window_add_shortcut(ui_window_t* win, uint32_t mods, uint8_t key,
                            ui_click_cb cb, void* userdata);
// match: (P2 & 0x07) == (mods & 0x07) && P1 == key; CapsLock diabaikan

// --- Dialog (async modal) ---
typedef void (*ui_dialog_cb)(void* userdata, int index);   // -1 = ESC/batal
void ui_dialog_show(ui_window_t* win, const char* title, const char* text,
                    const char* const* buttons, int n_buttons,
                    ui_dialog_cb cb, void* userdata);

// --- Notification ---
void ui_window_notify(ui_window_t* win, const char* text, uint32_t ms);

// --- Drag & Drop ---
typedef void (*ui_drop_cb)(void* userdata, const char* payload, int x, int y);
void ui_widget_set_draggable(ui_widget_t* widget, const char* payload);
void ui_widget_set_drop_target(ui_widget_t* widget, ui_drop_cb cb, void* userdata);

// --- Cursor ---
enum { UI_CURSOR_ARROW = 0, UI_CURSOR_IBEAM = 1, UI_CURSOR_HAND = 2 };
void ui_widget_set_cursor(ui_widget_t* widget, int kind);

// --- Settings ---
int ui_settings_save(ui_window_t* win);   // 1 sukses / 0 gagal
int ui_settings_load(ui_window_t* win);
```

## Kernel — Cursor (syscall 58)

Compositor menggambar satu-satunya kursor, selalu di atas backbuffer. Ketiga
bentuk berukuran **sama** 12×16 (`CURSOR_WIDTH×CURSOR_HEIGHT`) → clamping
`mouse_x/y` di `drivers/mouse.c` dan logika dirty-rect tidak berubah; hanya
selector bitmap yang berubah:

```c
static int g_cursor_kind = 0;
const uint8_t (*cbm)[12] = g_cursor_kind == 0 ? cursor_bitmap
                         : g_cursor_kind == 1 ? g_ibeam_bitmap
                         : g_hand_bitmap;   // 1 = putih, 2 = hitam (body lama)
```

```c
void kwm_set_cursor(int kind) {
    if (kind < 0 || kind >= 3) return;
    g_cursor_kind = kind;
    screen_mark_dirty(mouse_x, mouse_y, CURSOR_WIDTH, CURSOR_HEIGHT);  // repaint
}
```

`kernel/syscall.c` case 58 memvalidasi ulang kind (0..2) → `kwm_set_cursor`
→ return 0, else -1. Bitmap hand-made 12×16 (I-beam: batang vertikal dengan
serif; hand: jari menunjuk), dua warna: 1=putih, 2=hitam.

## Toolkit internal (libs/gui/widget/)

### Widget base — field baru + 3 setter

```cpp
bool draggable;      char* dnd_payload;      // DnD sumber (payload teks)
bool drop_target;    ui_drop_cb drop_cb;     void* drop_data;   // DnD target
int  cursor_kind;                            // UI_CURSOR_* per-widget

void set_draggable(const char* payload);     // strdup payload, draggable=true
void set_drop_target(ui_drop_cb cb, void* u);
void set_cursor(int kind);
virtual ~Widget() { _ui_free(dnd_payload); }
```

TextBox ctor → `cursor_kind = UI_CURSOR_IBEAM`; Button ctor →
`UI_CURSOR_HAND`. Window menyinkronkan ke kernel: di `track_hover()`, setelah
`hovered` dihitung → `want = hovered ? hovered->cursor_kind : ARROW`; bila
berubah → `sys_kwm_set_cursor(want)`. `~Window()` reset ke ARROW (app yang
tutup tidak meninggalkan I-beam/tangan).

### Clipboard — buffer global anonymous-namespace

`static char* g_clipboard` di `namespace {}` (dipakai TextBox via unqualified
call dan wrapper `extern "C"`). Set = strdup + free lama; get = return ptr;
clear = free + 0.

**TextBox::on_key** (sebelumnya `(void)mods`):
```cpp
if (mods & KEY_MOD_CTRL) {
    switch (ascii) {
    case 'c': case 'C': case 0x03: clipboard_set(text); break;          // salin seluruh isi
    case 'x': case 'X': case 0x18: clipboard_set(text); text[0]=0; cur=0; break;
    case 'v': case 'V': case 0x16: paste_at_cur (clamp MAX_TEXT); break;
    default: break;   // Ctrl+lain = shortcut app, bukan teks
    }
    return;
}
```
Penerimaan ganda (`'c'`/`0x03`, dst.) menutup dua kemungkinan mapping driver:
huruf dasar vs control-code. Tanpa konsep selection — copy = seluruh buffer.

### Shortcut — registry Window

```cpp
struct Shortcut { uint8_t mods, key; ui_click_cb cb; void* data; };
Shortcut shortcuts[16]; int n_shortcuts;
```
`run()` KEY_PRESS: setelah ESC, `else if (dialog)` (modal menelan ketikan),
`else` → scan registry **sebelum** dispatch ke widget fokus; match → fire cb,
`handled`, break. Cocok tak penuh → fall through ke `focused->on_key`.

### Dialog — widget overlay modal

Class `Dialog` (gaya Menu): `char* title, *text; char* btns[4]; int n_btns,
hover_btn; ui_dialog_cb cb; void* data; Window* win;`. Ukuran dihitung dari isi
(elemen terpanjang, min 220px): `h = 84 + nlines*16`. Geometri tombol via 3
helper `btn_x/btn_w/btn_row_y` (dipakai draw, hit-test, track_hover — DRY).
`draw`: panel `button_bg` + border `fg` 1px + judul `button_fg` + teks `fg` +
baris tombol (bg `theme.bg` agar kontras di panel biru, hover `button_hover`).

`Window`: `Dialog* dialog = 0;`. `open_dialog(d)`: center (sedikit di atas
tengah), render. `close_dialog()`: delete, null, render. `run()` CLICK-down:
`if (dialog)` → hanya `dialog->pick` (klik di luar diabaikan — modal blok latar);
MOVE → `track_hover` hanya menyentuh dialog; KEY_PRESS ESC: popup dulu →
`else if (dialog)` → `close_dialog()` lalu `cb(data, -1)`.

`Dialog::on_click` (out-of-class, pola `Menu::on_click`):
```cpp
int i = hit_button(mx, my);
if (i < 0) return;              // klik body dialog → tetap modal
ui_dialog_cb c = cb; void* d = data;
if (win) win->close_dialog();   // delete this — jangan sentuh member lagi
if (c) c(d, i);
```

### Notification — toast auto-expire

`Window`: `char* notify_text; uint64_t notify_until;`. `notify(text, ms)`:
strdup, `notify_until = sys_uptime() + ms`, render. `run()` tiap iterasi
(bangkit ~60/s via `sys_yield` + timer IRQ → cukup cek, tanpa timer infra):
`if (notify_text && sys_uptime() >= notify_until) notify_dismiss();`.
CLICK-down pada rect toast → dismiss (klik itu ditelan). `render()`: box
`(w-218, 8, 210, 28)` `button_bg` + border `fg`, digambar paling atas.

### Drag & Drop — klik-tahan jadi drag

`Window`: `Widget* drag_src; const char* drag_payload; int drag_x, drag_y;`.
- CLICK-down (tanpa popup/dialog): picked `draggable` → `drag_src = picked`,
  `drag_payload = picked->dnd_payload`, `drag_x/y = mouse` (**click_cb tidak
  dipanggil**); else jalur grab/on_click biasa.
- MOVE: jika `drag_src` → update `drag_x/y`, render (ghost); else drag/hover.
- CLICK-up: jika `drag_src` → `t = pick_bar() ?? root->pick(mx,my)`; bila
  `t && t->drop_target` → fire `t->drop_cb(t->drop_data, drag_payload, mx, my)`;
  `drag_src = 0`.
- `render()`: `draw_drag_ghost` = chip `button_hover` + border `accent` + teks
  payload, di offset (mx+4, my+4).

### Settings — persist theme ke KyuzenFS

```cpp
int settings_save() {
    int fd = sys_open("settings.ui", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    sys_write_fd(fd, &theme, sizeof(ui_theme_t));   // 6×uint32 = 24 byte
    sys_close(fd);
    return 1;
}
int settings_load() {
    int fd = sys_open("settings.ui", O_RDONLY);
    if (fd < 0) return 0;
    ui_theme_t t;
    int n = sys_read_fd(fd, &t, sizeof(ui_theme_t));
    sys_close(fd);
    if (n != (int)sizeof(ui_theme_t)) return 0;         // bukan theme valid
    if (!t.bg && !t.fg && !t.accent && !t.button_bg &&
        !t.button_fg && !t.button_hover) return 0;      // blob kosong
    set_theme(&t);
    return 1;
}
```
Blob biner polos, tanpa parsing — `ui_theme_t` fixed-width (struct C 6×uint32,
layout sama dgn `ui::Theme`). Load menolak file yang bukan theme (panjang ≠ 24
atau semua nol).

## Demo (apps/widget_demo.c)

Window 360×400 tetap. Tambahan:
- Toolbar: tombol **Dialog** (buka `ui_dialog_show` "Konfirmasi" Ya/Tidak) &
  **Notif** (toast 3 dtk).
- Menu Edit: **Salin**/**Tempel** → clipboard TextBox Kontrol.
- Shortcut **Ctrl+N** → dialog "Shortcut" (register `KEY_MOD_CTRL` + 'n' dan
  'N' — menutup dua kemungkinan kasus shift/caps).
- Tab **Setelan** baru (ScrollView-wrapped VBox — dogfood Phase 8): 3 preset
  tema (Gelap/Terang/Hijau), Simpan/Muat setelan, tombol Dialog + Notif,
  Salin/Tempel TextBox, chip draggable "Seret saya" (payload `chip:Setelan`) +
  drop zone "Jatuhkan di sini", label catatan kursor.

## Verifikasi

1. `make clean` + `make boot_image.iso` → build clean (kernel + semua apps + ISO).
2. `start widget_demo` → regresi Phase 8 utuh.
3. Kursor: hover TextBox → I-beam, tombol → tangan, off-widget → panah.
4. Shortcut: Ctrl+N → dialog; ketik normal tak terganggu.
5. Clipboard: Ctrl+C → Ctrl+V duplikat; menu Salin/Tempel jalan.
6. Dialog: modal, Ya/Tidak → status, klik luar diabaikan, ESC → Batal.
7. Notif: toast muncul, auto-expire ~3 dtk, klik → dismiss cepat.
8. DnD: seret chip ke drop zone → status payload; lepas di luar → tak fire.
9. Setelan: ganti tema → Simpan → Muat → tema balik.

## File

- `include/libui.h` — C ABI Phase 9 (clipboard/shortcut/dialog/notif/dnd/cursor/settings).
- `libs/gui/widget/` — toolkit: clipboard, Dialog class, toast, DnD, shortcut, cursor bridge, settings, wrapper extern "C".
- `kernel/gfx/compositor.c` — bitmap kursor + `kwm_set_cursor`.
- `kernel/syscall.c` — syscall 58.
- `include/gfx.h`, `libs/core/userlib.c`, `include/userlib.h` — deklarasi/wrapper cursor.
- `apps/widget_demo.c` — showcase Phase 9.
- `roadmap/GUI_ROADMAP.md` — Phase 9 → SELESAI.

## Belum ada (phase depan / batas sengaja)

- **Clipboard cross-app** — clipboard toolkit-global (satu app); cross-app butuh
  IPC kernel/shared memory.
- **DnD intra-window** — draggable widget tidak sekaligus fire `click_cb`
  (threshold-drag "seret langsung" adalah masa depan).
- **Dialog/Notification per-window overlay** — bukan window kernel terpisah
  (tidak bisa pindah / di-resize).
- **TextBox selection** — copy = seluruh buffer, tanpa anchor pilih.
- Kursor: bentuk hand-made 12×16; hot-spot, animasi, dan resize belum ada.
