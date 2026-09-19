# Desain: Phase 6 — Widget Toolkit `libui`: C ABI publik + Modern C++ di dalam

> **Status**: SELESAI (2026-08-06) — verifikasi runtime end-to-end via QEMU:
> `start widget_demo` → window render (KWM frame + label + button), hover,
> klik berulang (counter 0→1→2→3), ESC tutup & exit. Build clean (clang64).
> **Konteks**: Phase 5 memberi KWM + compositor + routing event window-local.
> Phase 6 adalah lapisan widget di atasnya. Keputusan kunci (user, 2026-08-06):
> **renderer tetap C (libgui), toolkit widget adalah Modern C++ baru di atasnya,
> Public API stabil C ABI** — supaya bahasa lain (C, Rust, Zig, dst.) cukup
> binding, tanpa bocor C++ ke kontrak publik.

## Arsitektur

```
Application (C / Rust / Zig / ...)
  ↓  Public GUI C ABI  (include/libui.h)     ← opaque handle, extern "C"
  ↓  extern "C" wrappers                     ← libs/widget/abi/libui_abi.cpp
Modern C++ Toolkit  (namespace ui)           ← Widget/Window/Button/Label/Layout/Painter/Theme
  ↓
libgui renderer (C, apps/libgui.c)           ← gui_create_window/draw_rect/draw_text
  ↓
KWM + Display Buffer (Phase 5)
```

- Public API: **tidak ada** C++ type, template, exception, RTTI, atau STL yang
  bocor ke luar. Semua symbol ekspor `extern "C"`. Handle opaque:
  `typedef struct ui_window ui_window_t;` — `ui_window_t*` di belakangnya
  adalah pointer objek `ui::*` (reinterpret_cast di wrapper).
- Prefiks `ui_` (bukan `gui_`) agar tidak tabrakan dengan `gui_window_t` di
  libgui.h yang sudah ada. libgui tetap renderer tingkat rendah; libui = pohon
  widget di atasnya.

## Batasan toolchain bare-metal (yang memaksa desain ini)

- **Tidak ada libstdc++/libc++.** `operator new/delete` (sized+unsized,
  array+non-array) di-stub ke `sys_alloc`/`sys_free` (userlib.o).
- **`-fno-exceptions -fno-rtti`.** `__cxa_pure_virtual` di-stub (`for(;;){}`).
- **ELF loader tidak menjalankan `.init_array`** → **TIDAK ada** global/static
  C++ object dengan constructor non-trivial. Semua object dibuat via `new`
  pada waktu jalan (runtime). `-fno-use-cxa-atexit` (tidak ada __cxa_atexit
  untuk global dtor), `-fno-threadsafe-statics` (tidak ada __cxa_guard_*).
- **Vtables di `.rodata`** (PT_LOAD R-X, app.ld), non-PIE base tetap
  `0x4000000` → relokasi selesai di link time, tidak butuh runtime reloc.
- `userlib.h`/`libgui.h` tidak punya `extern "C"` guard → dibungkus
  `extern "C" { ... }` di libs/widget/include/runtime/platform.hpp (dulu apps/libui.cpp).

## C ABI (include/libui.h)

Opaque handle: `ui_window_t`, `ui_widget_t`; `ui_theme_t` (bg, fg, accent,
button_bg, button_fg, button_hover — ARGB, alpha dipaksa `0xFF` di Painter);
`ui_click_cb(void* userdata)`.

```c
ui_window_t* w = ui_window_create(280, 140);
ui_widget_t* box = ui_vbox_create(w, 10);
ui_widget_t* lbl = ui_label_create(w, "Klik: 0");
ui_layout_add(box, lbl);
ui_widget_t* btn = ui_button_create(w, "+1");
ui_button_set_click(btn, on_click, 0);
ui_layout_add(box, btn);
ui_window_add(w, box);      // ke layout root (VBox margin 8px)
ui_window_run(w);           // blocking sampai window ditutup (ESC / X)
ui_window_destroy(w);
```

- Teks di-copy oleh toolkit (`_ui_strdup`) — caller boleh pakai stack buffer.
- Widget hasil `ui_*_create` adalah milik caller; `ui_window_destroy`
  membersihkan pohon (root VBox → anak-anak via `delete`).

## Toolkit internal (namespace ui)

- `Theme` — struct warna default gelap; diisi dari `ui_theme_t` via
  `Window::set_theme`.
- `Painter` — satu-satunya jembatan widget → renderer: `rect()` →
  `gui_draw_rect`, `text()` → `gui_draw_text`. Widget tidak pernah menyentuh
  framebuffer langsung (Prinsip #4 Renderer Agnostic).
- `Widget` — basis pohon: `x,y,w,h`, `visible`, `click_cb`/`userdata`,
  virtual `draw`, `set_hover`, `pick(mx,my)` (hit-test), `on_click`.
- `Label` — teks statis, `w = strlen*8`, `h = 16`; `set_text` menyalin + resize.
- `Button` — rect solid + teks, `hover` state; `w = strlen*8+16`, `h = 24`;
  klik → `click_cb(userdata)`.
- `Layout` — kontainer `children[16]`, virtual `arrange()`; `pick` anak
  **topmost-first**; `draw` = arrange lalu draw anak.
- `VBox` — susun anak vertikal berurutan (`y = cy; cy += h + spacing`).
- `Window` — `gui_window_t*` (libgui) + pohon root + event loop. Koordinat
  window-local **konten** (konsisten Phase 5C: `EVENT_*` membawa lokal).

## Event loop (Window::run)

```
render() awal
while running:
  sys_get_event(&ev):
    MOVE        → mouse_x/y = P1/P2; track_hover(); render bila hover berubah
    CLICK       → P3=mouse_x (lokal); jika left-down: pick() → on_click(); render()
    KEY_PRESS   → ESC (P1==27) → running=false
    WIN_CLOSE   → running=false
  sys_yield()
```

- Hover tracking: `track_hover()` membandingkan widget baru vs lama;
  perubahan memicu redraw (button hover warna).
- Event windows digambar ulang penuh (bg + pohon) — sederhana, cukup untuk
  ukuran window kecil. (Optimisasi dirty-rect per widget = nanti bila perlu.)

## Verifikasi (QEMU, screendump + PIL)

- Window render: frame KWM biru `0x1F4E8C` + tombol close merah `0xE53935`
  (Phase 5C), konten bg `0x121212`, label `0xE0E0E0`, button `0x0F3460`.
- Hover: cursor di atas button → warna berubah ke `0x2A4A7E` (button_hover).
- Klik: `mouse_button 1`/`0` (HMP) di tengah button → label "Klik: 0" →
  "Klik: 1" → "Klik: 2" → "Klik: 3" (callback C menaikkan counter +
  `ui_label_set_text` + re-render). Routing mouse KWM → CLICK window-local →
  `Widget::pick` → `Button::on_click` → `click_cb`.
- Tutup: `sendkey esc` → `EVENT_KEY_PRESS` → `running=false` →
  `ui_window_destroy` → `sys_exit`. Window hilang (bg/titlebar 0 piksel).

## File

- `include/libui.h` — Public C ABI (opaque handle, extern "C" guard).
- `libs/widget/` — toolkit C++ (namespace ui) + runtime stubs + wrapper extern "C" (dulu satu `apps/libui.cpp`, kini dipecah per-layer — lihat `widget-split.md`).
- `user_apps/widget_demo.c` — demo app C murni memakai C ABI.
- `user_apps/Makefile` — `CXX=clang++`, `CXXFLAGS_LIB`, target `widget_demo.elf`
  (link `widget_demo.o + userlib.o + libgui.o + libui.o`).
- `Makefile` (top) — shortcut `widget_demo.elf`; `boot_image.iso` ikut salin.
- `limine.conf` — module `widget_demo.elf` (auto-install ke KyuzenFS saat boot).

## Belum ada (Phase 7+)

TextBox, CheckBox, Slider, dll; layout non-VBox; repaint per-widget dirty rect;
style/font selain 8x16; keyboard input ke widget (tab focus). Semua sengaja
tidak dibangun sekarang (Prinsip #1 Start Simple, #2 Grow Naturally).
