# Desain: Phase 7 — Standard Widgets (TextBox, Image, CheckBox, Slider, ProgressBar)

> **Status**: SELESAI (2026-08-06) — build clean; verifikasi runtime manual oleh
> user (QEMU): semua 5 widget render di `widget_demo`, TextBox fokus/ketik,
> CheckBox toggle, Slider → ProgressBar, Image PNG tampil, +1/ESC regresi.
> **Konteks**: Phase 6 memberi toolkit `libui` (C ABI + Modern C++ di atas
> libgui) dengan Label/Button/VBox. Phase 7 menambahkan **Standard Widgets**
> sesuai roadmap: TextBox, Image, CheckBox, Slider, ProgressBar. Button/Label
> sudah ada sejak Phase 6.

## Tiga gap yang ditutup Phase 7

1. **Keyboard → widget (TextBox)**. Event loop Phase 6 hanya merutekan ESC.
   Keyboard sudah dirutekan KWM ke window fokus (Phase 5B) — toolkit tinggal
   memilih widget mana *di dalam* window yang menerima ketikan → **focus
   intra-window** (`Window::focused`).
2. **Drag (Slider)**. `EVENT_MOUSE_MOVE` selalu fire apa pun state tombol →
   toolkit melacak widget yang "dipegang" (`Window::grabbed`) sejak left-down,
   update posisi saat MOVE, lepas saat left-up (`EVENT_MOUSE_CLICK` P1==0,
   P2==0 — sebelumnya tidak ditangani).
3. **Piksel (Image)**. libgui tidak punya fungsi blit, tapi `gui_window_t.canvas`
   adalah member publik → apps menulis langsung (pola `blit_fit` viewer.c).
   PNG decode sudah ada di repo (`include/stb_image.h`, `STBI_ONLY_PNG`,
   memori wired ke `sys_alloc`/`sys_free`) → diekstrak jadi `apps/png.c` bersama.

## Perubahan file

| File | Perubahan |
|---|---|
| `include/libui.h` | Deklarasi C ABI 5 widget baru. |
| `apps/png.c` | **Baru** — decode PNG bersama: `png_decode(path,&w,&h) → uint32_t* XRGB8888` + `png_free`. |
| `libs/widget/` | 5 class widget + `Window::run` focus/drag/release + `Painter::image()` + extern `png_decode`. |
| `user_apps/widget_demo.c` | Demo diperluas: semua 5 widget + label/button (regresi). |
| `user_apps/Makefile` | `PNG_OBJ = png.o` + aturan compile + link `WIDGET_ELF` + `APP_OBJS`. |
| `roadmap/GUI_ROADMAP.md` | Phase 7 → SELESAI. |

**Tema tidak berubah** — widget baru memakai warna yang ada: TextBox fill
`button_bg`, border `accent`(fokus); CheckBox kotak `button_bg`, centang `fg`;
Slider track `button_bg`, handle `accent`; ProgressBar track `button_bg`, fill
`accent`. Tidak ada perubahan `ui_theme_t` → tanpa risiko kompatibilitas.

## C ABI (include/libui.h)

Semua callback memakai `ui_click_cb(void* userdata)` yang sudah ada.

```c
ui_widget_t* ui_textbox_create(ui_window_t* win, int width);       // h=24
void  ui_textbox_set_text(ui_widget_t* w, const char* text);
const char* ui_textbox_text(ui_widget_t* w);                        // copy internal
void  ui_textbox_set_enter(ui_widget_t* w, ui_click_cb cb, void* u);

ui_widget_t* ui_checkbox_create(ui_window_t* win, const char* label);
void  ui_checkbox_set_checked(ui_widget_t* w, int checked);
int   ui_checkbox_checked(ui_widget_t* w);
void  ui_checkbox_set_toggle(ui_widget_t* w, ui_click_cb cb, void* u);

ui_widget_t* ui_slider_create(ui_window_t* win, int min, int max);  // w=160,h=20
void  ui_slider_set_value(ui_widget_t* w, int value);
int   ui_slider_value(ui_widget_t* w);
void  ui_slider_set_change(ui_widget_t* w, ui_click_cb cb, void* u);

ui_widget_t* ui_progressbar_create(ui_window_t* win, int width);    // h=16, max=100
void  ui_progressbar_set_value(ui_widget_t* w, int value);          // clamp 0..100

ui_widget_t* ui_image_create(ui_window_t* win, const char* filename, int w, int h);
// rect widget w×h; PNG dimuat dari KyuzenFS saat create; nearest-neighbor scale
```

## Toolkit internal (libs/widget/)

### Widget — 3 virtual baru + flag focus

```cpp
bool has_focus;                                    // di-set Window saat focus berubah
virtual void set_focus(bool on) { has_focus = on; }
virtual bool focusable()        { return false; }           // TextBox → true
virtual bool on_drag(int mx, int my) { (void)mx; (void)my; return false; } // true = redraw
virtual void on_release() {}                                  // akhir drag
virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) {}
```

### Painter::image — satu metode baru (widget tetap gambar hanya via Painter)

Nearest-neighbor, tulis `win->canvas[(y+py)*win->width + x + q]` langsung
(pola `blit_fit` viewer.c). Tanpa clipping — rect widget selalu di dalam window.

### Window — focus + grab di event loop

```cpp
Widget* focused;   // focus keyboard intra-window (visual has_focus)
Widget* grabbed;   // widget yang memegang drag (left-down → release)

run():
  MOVE       → mouse_x/y = P1/P2
                if (grabbed && grabbed->on_drag(mx,my)) render();
                if (track_hover()) render();
  CLICK      → if (param1==0 && param2==1) {           // left down
                    grabbed = pick(mx,my);             // 0 boleh (klik bg)
                    set_focus(grabbed && grabbed->focusable() ? grabbed : 0);
                    if (grabbed) grabbed->on_click(mx,my);
                    render();
                } else if (param1==0 && param2==0) {   // left up (BARU)
                    if (grabbed) { grabbed->on_release(); grabbed = 0; }
                    render();
                }
  KEY_PRESS  → if (P1==27) running=false;
                else if (focused) { focused->on_key(P1, P3, P2); render(); }
  WIN_CLOSE  → running=false

set_focus(Widget* n): if (n==focused) return;
    if (focused) focused->set_focus(false);
    focused = n;
    if (focused) focused->set_focus(true);
```

### Kelas widget baru (gaya Label/Button yang ada)

- **TextBox**: `char text[256]`, `int cur`, `enter_cb`; `w` tetap, `h=24`,
  `focusable()=true`. Draw: rect `button_bg` + border 1px (`accent` jika fokus
  else `fg`), teks di (x+4,y+4), kursor `accent` 1×16 di `x+4+cur*8`.
  `on_key`: `ascii>=32` → sisip di kursor (cap 254); `scancode==0x0E`
  (Backspace) → hapus; `scancode==0x1C` (Enter) && `enter_cb` → panggil.
- **CheckBox**: `char* label`, `bool checked`, `toggle_cb`; `w=strlen*8+20`,
  `h=20`. Draw: kotak 12×12 `button_bg` + border `fg`, centang `accent` (dua
  segmen garis diagonal 1px) jika checked, label `fg` di (x+20,y+2).
  `on_click`: toggle + panggil `toggle_cb`.
- **Slider**: `int min,max,val`, `bool dragging`; `w=160,h=20`. Draw: track 4px
  `button_bg` tengah, handle 8×16 `accent` di `x + (val-min)*(w-8)/span`.
  `on_click`: clamp x→val, `dragging=true`, panggil `change_cb` bila val berubah.
  `on_drag`: clamp ulang jika dragging, return true (redraw). `on_release`:
  `dragging=false`.
- **ProgressBar**: `int val` (0..100); `w`, `h=16`. `set_value` clamp 0..100.
  Draw: track `button_bg`, fill `accent` lebar `val*w/100`.
- **Image**: `uint32_t* px; int iw,ih;` — `png_decode` di konstruktor; file
  hilang → `px=0` → placeholder rect `button_bg`. Draw: `p.image(x,y,w,h,px,iw,ih)`.

`png_decode`/`png_free` dideklarasikan di `libs/widget/include/primitives/image.hpp` sebagai
`extern "C" uint32_t* png_decode(const char*, int*, int*); extern "C" void png_free(uint32_t*);`

## apps/png.c (baru)

Konfigurasi stb_image disalin verbatim dari viewer.c:

```c
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MALLOC(sz)       sys_alloc(sz)
#define STBI_FREE(p)          sys_free(p)
#define STBI_REALLOC_SIZED(p,o,n) sys_realloc(p,o,n)
#include "stb_image.h"
```

`png_decode`: `sys_file_size` + `sys_alloc` + `sys_read_file_to_buffer` →
`stbi_load_from_memory(..., 4)` → konversi RGBA → XRGB8888
(`0xFF000000 | r<<16 | g<<8 | b`) → bebas buffer raw + stb → return buffer.
`png_free` → `sys_free`. Dipakai Image widget; dilink ke app yang memakainya.

## Demo (user_apps/widget_demo.c)

Window ±340×380, VBox spacing 10, memakai seluruh 5 widget + regresi label/button:

- Label "Klik: N" + Button "+1" (regresi Phase 6).
- Label "Teks: …" + **TextBox** (w 160) → Enter menyalin isi TextBox ke label.
- **CheckBox** "centang".
- **Slider** (0..100) + **ProgressBar** (w 160) → callback slider meng-set
  progressbar (membuktikan jalur callback end-to-end).
- **Image** "kyuzen.png" → 64×64.

## Verifikasi

1. `make` → build clean (kernel + semua apps).
2. `start widget_demo` → window render semua 5 widget.
3. TextBox: klik → border accent (fokus) → ketik "abc" → tampil + kursor; 
   Backspace hapus; Enter → label echo isi TextBox.
4. CheckBox: klik → glyph centang tampil.
5. Slider: drag handle → ProgressBar fill tumbuh proporsional.
6. Image: region 64×64 menampilkan kyuzen.png (bukan warna tema).
7. Regresi: "+1" masih menaikkan counter; ESC menutup.

## File

- `include/libui.h` — C ABI 5 widget baru.
- `apps/png.c` — decode PNG bersama (stb_image, XRGB8888).
- `libs/widget/` — toolkit C++: Painter::image, focus/grab, 5 widget, wrapper extern "C".
- `user_apps/widget_demo.c` — demo app C.
- `user_apps/Makefile` — `PNG_OBJ`, aturan `png.o`, link `WIDGET_ELF`.
- `Makefile` (top) — shortcut `widget_demo.elf`; `boot_image.iso` ikut salin (tidak berubah).

## Belum ada (Phase 8+)

ListView, TreeView, Table, ScrollView, Menu, MenuBar, Tab, Toolbar; fokus Tab
(keyboard navigasi antar widget); repaint per-widget dirty rect; font selain
8x16. Sengaja tidak dibangun sekarang (Prinsip #1 Start Simple, #2 Grow
Naturally).
