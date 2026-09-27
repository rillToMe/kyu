# KyuzenOS Display & GUI Roadmap (v2)

> **Status:** Planning
> **Objective:** Membangun fondasi Display System dan GUI Platform KyuzenOS secara bertahap tanpa overengineering.
> **Revisi dari v1:** menutup gap performa (dirty region, double buffering), memisahkan Display Buffer generik dari Line Buffer (Terminal), dan menambahkan Input Subsystem sebagai fondasi sebelum Window Manager.

---

# Filosofi

Sebelum membangun GUI modern, KyuzenOS harus memiliki **Display System** yang kuat.

GUI bukanlah langkah pertama.

Urutan yang benar:

```
Framebuffer
    ↓
Graphics
    ↓
Display System
    ↓
Input Subsystem
    ↓
Window Manager
    ↓
GUI Framework
    ↓
Desktop Environment
```

Jika Display System belum matang, Window, Widget, dan GUI akan sulit dikembangkan dan besar kemungkinan harus dirombak.

Perubahan di v2: **Input Subsystem** disisipkan sebelum Window Manager. Alasannya - WM butuh routing focus & click, dan itu butuh abstraksi event (keyboard/mouse) yang berdiri sendiri, bukan ditempel belakangan di Phase 5 seperti di v1.

Prinsip utama roadmap ini tetap:

- Bangun fondasi terlebih dahulu.
- Jangan membuat fitur yang belum dibutuhkan.
- Hindari overengineering - **tapi** jangan skip hal yang mahal untuk di-retrofit nanti (dirty region & double buffering masuk kategori ini, bukan "nice to have").
- API harus mudah berkembang tanpa breaking changes.

---

# Phase 1 - Framebuffer Driver ✅

Tidak berubah dari v1.

```
draw_pixel()
clear_screen()
```

---

# Phase 2 - Graphics Primitive ✅

Tidak berubah dari v1.

```
draw_rect()
draw_line()
draw_text()
draw_image()
```

---

# Phase 3 - Display System  ✅

## Tujuan

Memisahkan data yang ditampilkan dari framebuffer, dengan performa yang sudah benar sejak awal - bukan ditambal belakangan.

## Kenapa direstrukturisasi dari v1

Di v1, Phase 3 menggabungkan semua hal (Display Buffer, Viewport, scroll, repaint, Line Buffer) jadi satu daftar datar, dan dirty region ditandai opsional. Masalahnya:

- **Dirty region opsional → full repaint tiap frame.** Untuk satu viewport terminal, itu masih kerasa oke. Begitu masuk Phase 5 (Window Manager, banyak window overlapping), full repaint jadi mahal dan retrofit dirty region belakangan berarti nulis ulang jalur repaint yang sudah dipakai semua layer di atasnya.
- **Tidak ada double buffering** → tearing/flicker saat scroll atau repaint area besar, terutama kalau nanti render terjadi per-frame bukan per-event.
- **Line Buffer digabung dengan Display Buffer** → Line Buffer itu representasi teks (baris demi baris), sementara Window Manager nanti butuh buffer generik berbasis pixel/region, bukan baris. Kalau digabung sekarang, WM harus bikin abstraksi baru dari nol.

Solusinya bukan menambah kompleksitas, tapi **mengurutkan sub-tahap dengan benar** supaya tiap potongan tetap kecil dan sederhana.

## Struktur

```
Application

  ↓

Display Buffer (generik, pixel-based)

  ↓

Compositor tipis: Double Buffer + Dirty Region + Repaint

  ↓

Viewport (scroll offset, area terlihat)

  ↓

Framebuffer
```

Line Buffer (Terminal) dibangun **di atas** Display Buffer generik, bukan menggantikannya:

```
History (Terminal)

  ↓

Line Buffer  ──uses──▶  Display Buffer (generik)

  ↓

Viewport

  ↓

Framebuffer
```

Dengan begitu, aplikasi lain (nanti: Window Manager surface) bisa pakai Display Buffer yang sama tanpa lewat konsep "baris teks" yang cuma relevan buat terminal.

## Sub-Tahap

### 3A - Display Buffer (generik)

Buffer pixel murni, tidak tahu soal window, teks, atau scroll.

```c
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    ColorFormat format;
    uint8_t* pixels;
} DisplayBuffer;

DisplayBuffer* display_buffer_create(uint32_t w, uint32_t h, ColorFormat fmt);
void display_buffer_destroy(DisplayBuffer* buf);
void display_buffer_write_pixel(DisplayBuffer* buf, int32_t x, int32_t y, Color c);
```

**Keputusan yang harus dikunci di sini (jangan ditunda ke Phase 4):**
Siapa yang *owns* memory sebuah Display Buffer - dialokasikan oleh caller, atau oleh Display System sendiri lalu dipinjamkan (borrow)? Ini menentukan cara Window Manager nanti minta buffer per-window. Pilih salah satu sekarang, tulis sebagai komentar/dokumentasi di header, supaya Phase 5 tidak menebak-nebak.

### 3B - Compositing: Double Buffer + Dirty Region + Repaint

Ini bagian yang di v1 ditandai opsional. Di v2, **wajib**, karena jauh lebih murah dibangun sekarang (1 viewport, 1 aplikasi) daripada nanti (banyak window overlapping).

```c
typedef struct {
    int32_t x, y;
    uint32_t width, height;
} Rect;

typedef struct {
    Rect regions[MAX_DIRTY_REGIONS];
    uint32_t count;
} DirtyRegionList;

void dirty_region_mark(DirtyRegionList* list, Rect r);
void dirty_region_clear(DirtyRegionList* list);

// back buffer ditulis dulu, baru di-present ke framebuffer
// hanya area dirty yang di-blit
void screen_repaint(DisplayBuffer* back, DirtyRegionList* dirty);
void screen_present(DisplayBuffer* back);
```

Aturan sederhana: setiap `write_pixel`/`draw_*` ke Display Buffer otomatis memanggil `dirty_region_mark` untuk area yang disentuh. Repaint hanya memproses region itu, bukan seluruh layar.

### 3C - Viewport & Scroll Offset

```c
typedef struct {
    Rect bounds;        // area yang terlihat di layar
    int32_t scroll_x;
    int32_t scroll_y;
    DisplayBuffer* source;
} Viewport;

void viewport_scroll(Viewport* vp, int32_t dx, int32_t dy);
void viewport_render(Viewport* vp, DisplayBuffer* target_back_buffer);
```

Viewport hanya tahu cara memotong & menggeser area dari sebuah Display Buffer. Tidak tahu soal teks atau history - itu urusan layer di atasnya.

### 3D - Line Buffer (khusus Terminal, dibangun di atas 3A–3C)

```c
typedef struct {
    char** lines;
    uint32_t line_count;
    uint32_t capacity;
} LineBuffer;

void line_buffer_append(LineBuffer* lb, const char* text);
void line_buffer_render_to(LineBuffer* lb, DisplayBuffer* target, Viewport* vp);
```

Terminal history hidup di sini, terpisah total dari Display Buffer generik. Kalau nanti Window Manager butuh surface generik, dia pakai 3A–3C langsung, tanpa terseret konsep Line Buffer.

## Checklist Phase 3

- [x] 3A - Display Buffer generik (pixel-based, format warna, ownership model didokumentasikan) — `include/display.h`, `kernel/display.c`
- [x] 3A - **ADOPSI (2026-07-27)**: bukan lagi API mati — base_canvas/backbuffer/framebuffer dibungkus DisplayBuffer statis (`gfx_screen/back/fb_buffer()` di `fb.c`); semua primitive `draw_*` menggambar lewat DisplayBuffer layar; canvas window KWM = `DisplayBuffer*` owned (`display_buffer_create`)
- [x] 3B - Double Buffering (back buffer + present) — `compositor_flush` di `kernel/gfx/compositor.c`
- [x] 3B - Dirty Region tracking (**wajib**, bukan opsional) — `DirtyRegionList`, `screen_mark_dirty`
- [x] 3B - Screen Repaint (hanya area dirty) — komposit + present per-rect
- [x] 3B - **Kontrak auto-mark (2026-07-27)**: semua primitive `draw_*` menandai dirty SENDIRI (termasuk `draw_pixel`); `DisplayBuffer.dirty` opsional untuk buffer generik — caller tidak perlu `screen_mark_dirty` manual lagi
- [x] 3C - Viewport (bounds + scroll offset) — `viewport_render`/`viewport_scroll`
- [x] 3C - **ADOPSI (2026-07-27)**: `viewport_render` dioptimalkan (clipping per-panggilan + memcpy per baris) dan menjadi mesin blit compositor (base→back, back→fb) — komposit window alpha-mask tetap loop khusus
- [x] 3D - Line Buffer (Terminal, ring teks) — `drivers/tty.c`
- [x] 3D - Scroll History (Terminal) — `tty_scroll_view`, **terpicu mouse wheel (2026-07-27)**; fix shell: perintah tak dikenal tidak lagi `clear_screen` (dulu menghapus ring history)

## Target akhir Phase 3

- History tidak hilang
- Bisa scroll tanpa flicker (double buffer)
- Repaint murah karena cuma area dirty (bukan full-screen)
- Display Buffer generik siap dipakai ulang oleh Window Manager di Phase 5 - tanpa perlu dirombak

---

# Phase 4 - Input Subsystem (baru di v2) (Current Focus)

## Tujuan

Menyediakan abstraksi event input (keyboard, mouse) sebagai layer berdiri sendiri, sebelum Window Manager ada.

## Kenapa ini perlu jadi phase sendiri

Di v1, "Event" baru muncul di Phase 5 (GUI Framework) sebagai event widget (klik tombol, dsb). Tapi Window Manager di Phase (lama) 4 sudah butuh konsep **focus** dan **klik untuk pindah window** - dan itu nggak bisa jalan tanpa ada sumber event yang lebih dasar duluan (raw keyboard scancode → key event, mouse movement → mouse event). Kalau ini digabung ke dalam WM, WM jadi punya dua tanggung jawab sekaligus (routing window + decode input mentah), melanggar prinsip *Separation of Responsibility* yang sudah lu tulis sendiri di v1.

## Scope

- Keyboard event (key down/up, modifier)
- Mouse event (move, button down/up, scroll wheel)
- Event queue sederhana (polling atau interrupt-driven - pilih satu, jangan dua-duanya dulu)
- Belum tahu soal window/widget - cuma "event terjadi di koordinat (x, y)"

```c
typedef enum { EVT_KEY_DOWN, EVT_KEY_UP, EVT_MOUSE_MOVE, EVT_MOUSE_DOWN, EVT_MOUSE_UP, EVT_SCROLL } EventType;

typedef struct {
    EventType type;
    int32_t x, y;
    uint32_t keycode;
    int32_t scroll_delta;
} InputEvent;

bool input_poll_event(InputEvent* out);
```

## Checklist Phase 4

- [x] Keyboard driver → key event — **lengkap (2026-07-27)**: key down/up (`EVENT_KEY_PRESS`/`EVENT_KEY_RELEASE`), modifier Shift/Ctrl/Alt/CapsLock di P2, scancode di P3 — `drivers/keyboard.c`; detail: `DOCUMENTATION/design/gui-phase4-keyboard.md`
- [x] Mouse driver → mouse event — move/click sejak era KWM; **wheel (2026-07-27)**: IntelliMouse enable (paket 4 byte), `EVENT_SCROLL` (4)
- [x] Event queue — model TERPILIH & TERKUNCI: **interrupt-driven** (`kernel/event.c`, ISR push → syscall 29 pop)
- [x] Integrasi: scroll Terminal (Phase 3D) terpicu mouse wheel (2026-07-27) — routing: tidak ada window aktif → `tty_scroll_view`; ada window → `EVENT_SCROLL` ke app

## Catatan

Ini tetap tahap kecil - jangan bangun input manager yang njelimet. Cukup: driver → event → queue → poll. Routing event ke window spesifik itu tanggung jawab Phase 5, bukan di sini.

---

# Phase 5 - Window Manager

> **Status: SELESAI (2026-08-05)** — 5A spawn ✅ · 5B routing ✅ · 5C focus+dekorasi ✅ · 5D Alt-Tab ✅ (implementasi; verifikasi interaktif menyusul).
> **Roadmap detail: `PHASE5_WM_ROADMAP.md`** — sub-tahap 5A–5D (spawn → routing → focus+dekorasi → polish), 12 keputusan terkunci (2026-07-27).
> **Design doc: `DOCUMENTATION/design/gui-phase5-wm.md`** — geometri frame (canvas=konten), dekorasi compositor, koordinat window-local, protokol close, Alt-Tab.

## Tujuan


yang paling utama itu OS harus strukture nya seperti ini. applikasi tidak akan bisa langsung berkomunikasi ke kernel, harus ada perantaranya.

                 Kernel (Ring 0)
────────────────────────────────────────────
 Scheduler
 Memory Manager
 VFS
 KyuzenFS
 Network
 Input Driver
 Framebuffer/DRM
 IPC
 Surface Manager (buffer & event)
────────────────────────────────────────────

                 User Space (Ring 3)
────────────────────────────────────────────
 init
 login
 desktopd
 kwm (Window Manager / Compositor)
 notificationd
 themed
 launcher
────────────────────────────────────────────

                 Toolkit
────────────────────────────────────────────
 libkyuzen
 libui
 libwidget
────────────────────────────────────────────

                 Applications
────────────────────────────────────────────
 File Manager
 Browser
 Terminal
 Editor
 Settings
 Game
────────────────────────────────────────────

Mampu memiliki beberapa window.

```
Desktop
├── Terminal
├── Explorer
└── Calculator
```

Window Manager bertugas:

- posisi window
- ukuran
- z-order
- focus
- clipping
- redraw
- **routing InputEvent (dari Phase 4) ke window yang tepat berdasarkan posisi/focus** *(baru - sekarang ada sumber event yang jelas untuk di-routing)*

Window Manager belum mengenal Button ataupun Widget. Window hanya Surface - dan Surface itu, secara teknis, adalah satu instance Display Buffer dari Phase 3A.

---

# Phase 6 - GUI Framework (Modern C++)

> **Status: SELESAI (2026-08-06)** — Widget Toolkit `libui`: C ABI publik
> (opaque handle) + implementasi Modern C++ di atas libgui. Verifikasi runtime
> end-to-end via QEMU: `start widget_demo` → window + label + button render,
> hover, klik → callback → `ui_label_set_text` (counter 0→1→2→3), ESC tutup.
> Detail: `DOCUMENTATION/design/gui-phase6-toolkit.md`.

## Tujuan

Framework GUI berbasis Modern C++, di atas Window Manager.

```
Application (C / Rust / Zig / ...)
  ↓  Public GUI C ABI (include/libui.h)   ← opaque handle, extern "C"
  ↓  extern "C" wrappers
Widget Toolkit (Modern C++, namespace ui)
  ↓  Widget / Window / Button / Label / Layout / Painter / Theme
libgui renderer (C, apps/libgui.c)
  ↓
KWM + Display Buffer (Phase 5)
```

Mengenal:

- Window
- Widget
- Event widget (klik tombol, dsb - dibangun di atas InputEvent dari Phase 4, bukan menggantikannya)
- Layout
- Theme
- Painter

**Keputusan arsitektur (2026-08-06):** renderer tetap C (libgui); toolkit
widget adalah layer Modern C++ baru di atasnya; Public API **stabil C ABI** —
handle opaque, tidak ada C++ type/template/exception/RTTI/STL yang bocor, semua
symbol ekspor `extern "C"`. Bahasa lain cukup binding ke C ABI.

**Batasan toolchain bare-metal:** tidak ada libstdc++ → `operator new/delete`
di-stub ke `sys_alloc/sys_free`; `-fno-exceptions -fno-rtti` →
`__cxa_pure_virtual` di-stub; ELF loader tidak menjalankan `.init_array` →
tidak ada global/static C++ object dengan constructor non-trivial (semua
object dibuat via `new` saat runtime); vtables di `.rodata` (PT_LOAD R-X),
non-PIE base tetap `0x4000000` → relokasi selesai di link time.

**Widget saat ini (Phase 6):** `ui_window_t`, `ui_label_t`, `ui_button_t`,
`ui_vbox_t` + tema (`ui_theme_t`) + callback klik (`ui_click_cb`). Layout
hierarkis (VBox, hit-test topmost-first), hover state, koordinat window-local
konten (konsisten dengan keputusan Phase 5C).

---

# Phase 7 - Standard Widgets

> **Status: SELESAI (2026-08-06)** — TextBox, Image, CheckBox, Slider,
> ProgressBar ditambahkan ke toolkit `libui` (Button/Label sudah ada sejak
> Phase 6). TextBox = widget pertama yang terima ketikan (focus intra-window);
> Slider memperkenalkan drag (grab/on_drag/release di `Window::run`); Image
> memakai decode PNG bersama (`apps/png.c`, stb_image `STBI_ONLY_PNG`,
> XRGB8888) yang digambar lewat `Painter::image()` (nearest-neighbor,
> `gui_window_t.canvas`). Build clean; verifikasi runtime manual oleh user
> (QEMU): `start widget_demo` → semua 5 widget render, TextBox fokus/ketik,
> CheckBox toggle, Slider → ProgressBar, Image tampil, +1/ESC regresi.
> Detail: `DOCUMENTATION/design/gui-phase7-widgets.md`.

---

# Phase 8 - Advanced Widgets

> **Status: SELESAI (2026-08-06)** — ListView, TreeView, Table, ScrollView,
> Menu, MenuBar, Tab, Toolbar ditambahkan ke toolkit `libui`. Tiga kapabilitas
> baru: clipping di `Painter` (libgui tak punya scissor), routing roda
> `EVENT_SCROLL` di `Window::run`, dan popup/overlay untuk menu dropdown.
> `Scrollable` base menyatukan scroll roda + drag thumb untuk 4 widget;
> `Window::add_bar` menaruh MenuBar/Toolbar full-width. Build clean; verifikasi
> runtime manual oleh user (QEMU): menu dropdown open/hover/switch/ESC/dismiss,
> toolbar, switch tab, scroll + thumb drag, expand/collapse pohon, regresi
> Phase 7. Detail: `DOCUMENTATION/design/gui-phase8-widgets.md`.

*(Catatan v1 "ScrollView reuse Viewport dari Phase 3C" berlaku sebagai konsep —
toolkit userspace tak bisa menyentuh Viewport kernel, jadi scroll/clip dibangun
di sisi toolkit (`Painter::clip` + `Scrollable`), bukan reuse Viewport.)*

---

# Phase 9 - Desktop Services

> **Status: SELESAI (2026-08-06)** — Clipboard, Dialog, Notification, Drag &
> Drop, Cursor, Shortcut, Settings ditambahkan ke toolkit `libui`. Keyboard
> P2 (modifier) yang dulu dibuang widget kini dipakai: shortcut registry
> (diperiksa sebelum dispatch ke widget fokus) + TextBox **Ctrl+C/V/X** ke
> clipboard. Dialog = overlay modal async (`ui_dialog_show` + `ui_dialog_cb`
> — Ya/Tidak/ESC), Notification = toast auto-expire via `sys_uptime` + klik
> dismiss. Drag & Drop memakai kontrak klik yang ada (klik-tahan → ghost →
> drop ke `drop_target`). Cursor butuh satu syscall kernel (**58
> `sys_kwm_set_cursor`**, 3 bitmap 12×16 di compositor: panah/I-beam/tangan;
> per-widget via `ui_widget_set_cursor`, TextBox=I-beam, Button=tangan, reset
> ARROW saat window tutup). Settings persist theme ke KyuzenFS
> (`settings.ui`, blob 6×uint32). Build clean; verifikasi runtime manual oleh
> user (QEMU): `start widget_demo` → kursor per-widget, Ctrl+N shortcut →
> dialog, dialog Ya/Tidak/Batal, toast 3 dtk, clipboard Ctrl+C/V/X, drag chip
> → drop zone, tema → Simpan/Muat, regresi Phase 8 utuh.
> Detail: `DOCUMENTATION/design/gui-phase9-desktop-services.md`.

---

# Phase 10 - Desktop Environment

> **Status: SELESAI (2026-08-06)** — shell desktop GUI menggantikan CLI
> sebagai wajah utama boot (login → `sys_spawn("desktop.elf")` + shell CLI
> tetap hidup di background). Window **desktop** kernel (full-screen,
> frameless, z=0, no-focus) + titlebar berjudul + window list (syscall
> 59–62) + `sys_get_screen_size` (63). App baru **Terminal** (command loop
> userspace di TextEdit readonly) & **Setelan** (tema + Simpan/Muat + info
> sistem); **semua 6 app lama diport ke libui** (Explorer Table, Image
> Viewer + zoom, Kalkulator grid, Text Editor TextEdit, Jam tick 1Hz,
> Task Manager ProgressBar tick). `MAX_TASKS` 8 → 16. Build clean;
> verifikasi runtime manual oleh user (QEMU): desktop + launcher + taskbar
> klik → bring-to-front, Setelan tema, Terminal `ls`/`fetch`/`start clock`,
> regresi `widget_demo`.
> Detail: `DOCUMENTATION/design/gui-phase10-desktop-env.md`.

Tidak berubah dari v1: Explorer, Terminal, Calculator, Settings, Image Viewer, Text Editor.

---

# Design Decisions to Lock (baru di v2)

Beberapa keputusan kecil tapi mahal kalau ditunda - sebaiknya dikunci sebelum pindah phase, ditulis satu-dua kalimat di dokumentasi kode:

| Keputusan | Kunci sebelum | Kenapa |
|---|---|---|
| Ownership model Display Buffer (siapa alloc/free) | Phase 5 | WM perlu tahu cara minta buffer per-window |
| Model event queue (polling vs interrupt) | Phase 5 | WM routing bergantung pada model ini |
| Color format konsisten di semua layer | Phase 3 selesai | Konversi format di tengah jalan itu sumber bug yang sulit dilacak |
| Koordinat: global screen-space vs local ke Viewport/Window | Phase 5 | Menentukan cara clipping & hit-testing dihitung |

Ini bukan dokumen desain formal - cukup catatan singkat supaya tidak ada asumsi diam-diam yang beda antar layer.

---

# Design Principles

## 1. Start Simple

Mulai dari implementasi sederhana. Jangan membuat abstraction yang belum diperlukan.

## 2. Grow Naturally

Framework berkembang mengikuti kebutuhan. Jika suatu fitur belum digunakan, jangan dibuat.

## 3. Separation of Responsibility

| Layer | Tanggung jawab |
|---|---|
| Graphics Library | menggambar |
| Display System | menyimpan & mengomposisikan tampilan |
| Input Subsystem | menerjemahkan input mentah jadi event |
| Window Manager | mengatur window & routing event |
| GUI Framework | mengatur widget |
| Applications | logika aplikasi |

## 4. Renderer Agnostic

Widget tidak mengetahui framebuffer. Widget hanya menggambar lewat Painter. Painter meneruskan ke renderer. Renderer bisa diganti tanpa mengubah widget.

## 5. Public API

GUI nantinya memiliki C ABI. Implementasi internal Modern C++. Bahasa lain (C, Rust, Zig, Odin, Nim) cukup binding ke Public API.

## 6. Performance Budgeted Early (baru di v2)

Dirty region dan double buffering bukan optimisasi belakangan - itu bagian dari kontrak layer Display System. Kalau layer di atasnya (WM, GUI Framework) mengasumsikan repaint itu murah, tapi ternyata fondasinya full-repaint, semua layer di atas ikut lambat tanpa cara mudah memperbaikinya tanpa rombak total.

---

# Current Priority

Prioritas saat ini: Phase 10 (Desktop Environment: desktop shell + Terminal & Settings + port 6 app ke libui) **SELESAI (2026-08-06)**. Detail: `DOCUMENTATION/design/gui-phase10-desktop-env.md`.

- [x] 3A - Display Buffer generik
- [x] 3B - Double Buffering
- [x] 3B - Dirty Region (wajib)
- [x] 3B - Screen Repaint
- [x] 3C - Viewport
- [x] 3D - Line Buffer (Terminal)
- [x] 3D - Scroll History

Catatan implementasi 3D: scrollback terminal berbasis **baris teks** (ring buffer di `drivers/tty.c`), bukan lewat Viewport pixel 3C — Viewport tetap untuk surface Window Manager di Phase 5 sesuai rencana. Scroll history kini terpicu **mouse wheel** (2026-07-27).

Catatan adopsi (2026-07-27): 3A/3C yang semula API-only kini benar-benar dipakai pipeline — primitive `draw_*` lewat DisplayBuffer layar (auto-mark dirty), compositor blit via `viewport_render`, surface KWM = `DisplayBuffer` owned per window. Detail: `DOCUMENTATION/design/gui-phase3-adoption.md`.

Setelah Phase 3 selesai → Phase 4 (Input Subsystem) **SELESAI (2026-07-27)** — keyboard lengkap (key up + modifier), mouse + wheel, event queue interrupt-driven. Detail keyboard: `DOCUMENTATION/design/gui-phase4-keyboard.md`. → Phase 5 (Window Manager) **SELESAI (2026-08-05)** — KWM + dekorasi + routing event window-local + Alt-Tab. Detail: `DOCUMENTATION/design/gui-phase5-wm.md`. → Phase 6 (GUI Framework) **SELESAI (2026-08-06)** — Widget Toolkit libui, C ABI + Modern C++, verifikasi runtime QEMU. Detail: `DOCUMENTATION/design/gui-phase6-toolkit.md`. → Phase 7 (Standard Widgets: TextBox, Image, CheckBox, Slider, ProgressBar) **SELESAI (2026-08-06)** — focus intra-window, drag Slider, Image PNG (stb_image), verifikasi manual user. Detail: `DOCUMENTATION/design/gui-phase7-widgets.md`. → Phase 8 (Advanced Widgets: ListView, TreeView, Table, ScrollView, Menu, MenuBar, Tab, Toolbar) **SELESAI (2026-08-06)** — clipping `Painter` + `Scrollable` base (roda + thumb drag), popup menu dropdown, bar full-width (`add_bar`), verifikasi manual user. Detail: `DOCUMENTATION/design/gui-phase8-widgets.md`. → Phase 9 (Desktop Services: Clipboard, Dialog, Notification, Drag & Drop, Cursor, Shortcut, Settings) **SELESAI (2026-08-06)** — clipboard + Ctrl+C/V/X, shortcut registry, dialog async modal, toast auto-expire, drag & drop, cursor per-widget (syscall 58), settings persist theme; verifikasi manual user. Detail: `DOCUMENTATION/design/gui-phase9-desktop-services.md`. → Phase 10 (Desktop Environment: desktop shell + Terminal & Settings + port 6 app ke libui) **SELESAI (2026-08-06)** — window desktop kernel (z=0 frameless no-focus), titlebar berjudul + window list (syscall 59–62) + `sys_get_screen_size` (63, fix 2-pointer), TextEdit widget + tick callback + image zoom, login → desktop + shell CLI background, `MAX_TASKS` 16; verifikasi manual user. Detail: `DOCUMENTATION/design/gui-phase10-desktop-env.md`.

---

# Long-Term Vision

Target akhir bukan sekadar membuat library GUI. Target akhirnya adalah membangun sebuah Desktop Platform yang memiliki arsitektur bersih, modular, ringan, dan dapat berkembang selama bertahun-tahun tanpa perlu dirombak ulang.

KyuzenOS akan dibangun dari fondasi yang kuat, bukan dari kumpulan fitur yang ditambahkan secara acak.