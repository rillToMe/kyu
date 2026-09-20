# Desain: Pemecahan Toolkit Widget `libs/widget/`

> **Status**: SELESAI (2026-09-19).
> **Konteks**: `apps/libui.cpp` adalah satu file C++ 3.798 baris berisi seluruh
> toolkit widget (namespace `ui`) + runtime shim + wrapper C ABI. Dokumen ini
> mencatat pemecahannya menjadi `libs/widget/` **tanpa mengubah perilaku satu
> piksel pun** — murni pemindahan kode (refactor mekanis), bukan penulisan ulang.
> **Verifikasi**: `make apps` (bersih dari nol), `make test-textedit`,
> `make test-libui-theme`, `make` (kernel), `make boot_image.iso`, dan diff
> symbol table ELF lama vs baru. Semua lolos (lihat bagian Verifikasi).

## Kenapa dipecah

- Satu file 3,8k baris menyulitkan review: perubahan kecil di satu widget
  membuat diff menyentuh file yang sama dengan seluruh toolkit lain.
- Batas arsitektur tidak terlihat sama sekali — `Window` (composition root),
  widget primitif, dan wrapper ABI berada di file yang sama.
- Struktur folder berlayer memberi tempat yang jelas untuk aturan yang sudah
  ada tapi hanya "terlihat" lewat urutan baris (mis. `Widget::draw` butuh
  `Painter`, `Dialog` butuh `Window` lengkap).

## Layout akhir

```
libs/widget/
├── include/
│   ├── runtime/     platform.hpp  memory.hpp
│   ├── core/        theme.hpp  painter.hpp  widget.hpp
│   ├── primitives/  label.hpp  button.hpp  textbox.hpp  checkbox.hpp
│   │                slider.hpp  progressbar.hpp  image.hpp
│   ├── editor/      textedit.hpp
│   ├── layout/      layout.hpp  vbox.hpp  hbox.hpp
│   ├── containers/  scrollable.hpp  scrollview.hpp  listview.hpp
│   │                table.hpp  treeview.hpp  tab.hpp
│   ├── chrome/      toolbar.hpp  menu.hpp  menubar.hpp  statusbar.hpp
│   ├── dialog/      dialog.hpp  promptdialog.hpp
│   ├── window/      window.hpp
│   └── services/    clipboard.hpp
├── src/             (mencerminkan include/: satu .cpp per .hpp, +
│   │                 runtime/runtime.cpp, services/clipboard.cpp)
│   └── …
└── abi/             libui_abi.cpp      ← blok `extern "C"` C ABI publik
```

Satu class = satu pasang `.hpp`/`.cpp`. **Body method tetap inline di header**
(dipindah apa adanya, tidak ditulis ulang menjadi definisi out-of-line), jadi
`.cpp` yang tidak punya definisi out-of-class hanya berisi `#include` — file itu
sekaligus jadi bukti header-nya bisa dikompilasi mandiri.

## Aturan dependency antar layer

```
abi/libui_abi.cpp              ← jembatan extern "C" (satu-satunya reinterpret_cast)
        ↓
window/window.hpp              ← composition root (boleh #include hampir semua)
        ↓
containers/  chrome/  dialog/  ← forward-declare `class Window;`, TIDAK #include window.hpp
        ↓
primitives/  layout/           ← cuma depend ke core/
        ↓
core/ (theme.hpp, painter.hpp, widget.hpp)   ← tidak depend ke layer atas
```

- `core/widget.hpp` forward-declare `class Window;` (member `Widget::owner`) dan
  `class Painter;` (`virtual void draw(Painter&)`) — tidak meng-include apa pun
  dari layer atas.
- `core/painter.hpp` tidak tahu apa-apa soal `Widget`.
- Header `containers/`, `chrome/`, `dialog/` hanya forward-declare `Window`;
  **hanya** `.cpp` yang benar-benar memanggil method `Window` (plus
  `window/window.hpp` sendiri) yang meng-include `window/window.hpp`.
- `window/window.hpp` boleh meng-include implementasi lengkap semua yang dia
  pakai — dia composition root, itu normal.

## Ke mana bagian "global" dari file lama pindah

| Dulu (satu translation unit) | Sekarang |
|---|---|
| `namespace { _ui_alloc/_ui_free/_ui_strlen/_ui_strncmp/_ui_strdup }` | `include/runtime/memory.hpp` (deklarasi) + `src/runtime/runtime.cpp` (definisi). Linkage naik dari anonim → C++ biasa supaya bisa dipakai lintas file |
| `operator new` (2 varian) / `operator delete` (4 varian) + `__cxa_pure_virtual` | `src/runtime/runtime.cpp` — **tetap persis sekali** di seluruh link (ODR) |
| `g_clipboard` + `clipboard_set/get/clear` | `include/services/clipboard.hpp` + `src/services/clipboard.cpp` (`g_clipboard` tetap `static` + zero-initialized) |
| `extern "C" { userlib.h; libgui.h; color_*.h }` di kepala file | `include/runtime/platform.hpp` — satu tempat untuk semua file (header itu tidak punya guard `extern "C"` sendiri) |
| `extern "C" png_decode/png_free` | `include/primitives/image.hpp` (hanya `Image` yang memakainya) |
| `SHADE_5/10/18/55` | `include/core/theme.hpp` |
| `SETTINGS_TAG*`, `tag_match()`, `theme_empty()` | `include/window/window.hpp` (tetap `constexpr`/`static`; hanya `Window::settings_save/load` yang memakainya) |
| `UI_ACCENT_PREFIX` + `UI_ACCENT_PREFIX_LEN` | `include/dialog/dialog.hpp` |
| 6 definisi out-of-class yang butuh `Window` lengkap | `src/core/widget.cpp` (`Widget::set_visible`), `src/dialog/dialog.cpp` (`Dialog::on_click`), `src/dialog/promptdialog.cpp` (`on_cancel/on_key/on_click`), `src/chrome/menu.cpp` (`Menu::on_click`), `src/chrome/menubar.cpp` (`MenuBar::on_click/draw`) |

Tidak ada global/static object baru dengan constructor non-trivial: ELF loader
KyuzenOS tidak menjalankan `.init_array`, jadi invarian lama tetap berlaku.

## Peta file lama → baru

Rentang baris mengacu ke `apps/libui.cpp` sebelum pemecahan.

| File baru | Isi (baris lama) |
|---|---|
| `src/runtime/runtime.cpp` | 49–71 (runtime shim), 89–101 (operator new/delete + `__cxa_pure_virtual`) |
| `src/services/clipboard.cpp` | 77–85 |
| `include/core/theme.hpp` | 105–169 (`Theme`), 171–186 (`SHADE_*`) |
| `include/core/painter.hpp` | 208–383 |
| `include/core/widget.hpp` | 385–504 |
| `include/primitives/label.hpp` | 506–524 |
| `include/primitives/button.hpp` | 526–561 |
| `include/primitives/textbox.hpp` | 563–624 |
| `include/editor/textedit.hpp` | 626–1179 |
| `include/primitives/checkbox.hpp` | 1181–1215 |
| `include/primitives/slider.hpp` | 1217–1265 |
| `include/primitives/progressbar.hpp` | 1267–1285 |
| `include/primitives/image.hpp` | 1287–1333 (+ deklerasi `png_decode`/`png_free`, baris 42–45) |
| `include/layout/layout.hpp` | 1335–1385 |
| `include/layout/vbox.hpp` | 1387–1403 |
| `include/layout/hbox.hpp` | 1405–1424 |
| `include/containers/scrollable.hpp` | 1426–1511 |
| `include/containers/scrollview.hpp` | 1513–1688 |
| `include/containers/listview.hpp` | 1690–1765 |
| `include/containers/table.hpp` | 1767–1869 |
| `include/containers/treeview.hpp` | 1871–1975 |
| `include/containers/tab.hpp` | 1977–2047 |
| `include/chrome/toolbar.hpp` | 2049–2102 |
| `include/chrome/menu.hpp` | 2104–2108 (judul section), 2245–2381 |
| `include/dialog/dialog.hpp` | 2109–2243 |
| `include/chrome/menubar.hpp` | 2383–2438 |
| `include/chrome/statusbar.hpp` | 2440–2472 |
| `include/dialog/promptdialog.hpp` | 2474–2551 |
| `include/window/window.hpp` | 188–206 (`SETTINGS_*` + helper), 2553–3086 |
| `abi/libui_abi.cpp` | 3162–3800 (blok `extern "C"` C ABI) |

## Build

| Jalur | Wiring |
|-------|--------|
| User apps | `user_apps/Makefile`: `WIDGET_SRCS = libs/widget/src/*/*.cpp + abi/libui_abi.cpp`, objek di `libs/widget/build/` (di-gitignore), di-link via `$(LIBUI_OBJ)`. Urutan link bebas — tidak ada static ctor |
| Rule kompilasi | `$(WIDGET_BUILD)/%.o: $(WIDGET_DIR)/%.cpp` + `-I$(WIDGET_DIR)/include -I$(INC_DIR)` |
| `make clean` (apps) | ikut `rm -rf libs/widget/build` |
| Host test TextEdit | `make test-textedit` — sumber `libs/widget/**/*.cpp` di-link apa adanya, 20 simbol (syscall/libgui/png) di-stub |
| Host test tema/render | `make test-libui-theme` — header per-layer di-include untuk periksa tipe internal, objek toolkit di-link; render dicek piksel-per-piksel |
| Kernel | tidak berubah — `SRC_DIRS` hanya mengambil `*.c`, toolkit tidak pernah bagian dari `myos.bin` |

`include/libui.h` (C ABI publik) **tidak berubah** — aplikasi yang sudah memakai
libui.h tidak perlu menyesuaikan cara pakainya.

## Verifikasi

| # | Uji | Hasil |
|---|-----|-------|
| 1 | Compile bersih dari nol (`make -C user_apps clean && make -C user_apps all`) | 20 ELF terbentuk, 0 error, 0 warning; tidak ada "multiple definition of operator new/delete" |
| 2 | `make test-textedit` | **48 PASS, 0 FAIL** (undo/redo, seleksi+clipboard, find/replace, aritmetika word wrap, readonly, cap 8K) |
| 3 | `make test-libui-theme` | **37 PASS, 0 FAIL** (tema opaque, gradien tombol piksel-per-piksel vs jalur `aa_shade` lama, settings v0/v1, viewer fit/zoom/center) |
| 4 | Warning `-Wall -Wextra`, lama vs baru | 0 vs 0 — tidak ada warning baru |
| 5 | Diff symbol ELF `notepad.elf` (lama 1-file vs baru) | **ABI publik identik: 115/115 symbol `ui_*`** — tidak ada yang hilang/berubah |
| 6 | `.data`/`.bss` | identik (2217/116 byte); `.text` 138.942 → 131.902 byte (lebih kecil, inlining lintas TU) |
| 7 | `make` (kernel) + `make boot_image.iso` | kernel up-to-date; ISO 12,6 MB tergenerate + Limine BIOS terpasang |
| 8 | Invarian ekstraksi | setiap baris tak-kosong dari `namespace ui` lama (105–3160) mendarat tepat sekali di file baru |

Satu-satunya delta symbol global adalah 8 helper yang **memang** naik ke linkage
eksternal supaya bisa dipakai lintas file: `_ui_alloc/_ui_free/_ui_strlen/
_ui_strncmp/_ui_strdup`, `clipboard_set/get/clear`. ABI publik `ui_*` tidak
tersentuh.

## Deviasi yang disengaja

1. **`include/runtime/platform.hpp` (file tambahan).** `userlib.h`/`libgui.h`/
   `libs/color` tidak punya guard `extern "C"`; file lama membungkusnya sekali di
   kepala TU. Pindah ke satu header bersama lebih aman daripada menyalin blok itu
   ke puluhan file.
2. **`runtime/memory.hpp` ditaruh di `include/runtime/`** — pohon `include/` yang
   diminta tidak menyebut folder `runtime/`, tetapi header ini memang privat
   toolkit (bukan bagian C ABI).
3. **`SHADE_*` dan `SETTINGS_*` dipisah.** Aslinya berdampingan di blok "Warna";
   dipisah ke `core/theme.hpp` dan `window/window.hpp` sesuai pemakainya.
4. **22 `.cpp` hanya berisi `#include`.** Karena seluruh body method tetap inline
   di header (dipindah apa adanya), file-file itu tidak punya definisi
   out-of-class. Memindahkan body ke out-of-line adalah fase 2 terpisah.
5. `libs/widged/` (folder kosong, salah ketik, sudah ada sebelum pemecahan) sudah
   dihapus — tidak ada Makefile/script yang mereferensikannya.

## Kejanggalan kode lama

Sesuai aturan "jangan ubah logika", temuan ini dicatat dulu tanpa disentuh.
Empat di antaranya dijadikan ticket terpisah dan sudah diperbaiki, masing-masing
satu commit supaya bisa di-revert sendiri-sendiri:

| Temuan | Status |
|---|---|
| `Slider::clamp_to()` membagi dengan `w - 8`; slider dengan `w == 8` → division by zero (SIGFPE, atau nilai garbage / UB) | **fixed** `0654b73` — konstanta `Slider::HANDLE_W` dipakai `draw()`+`clamp_to()`, dan `clamp_to()` keluar lebih awal saat tak ada ruang gerak (nilai dibiarkan tetap, `w` caller tidak dipaksa) |
| `Table::clear()` membebaskan sel tapi tidak mereset `selected`/`hover_row` → indeks baris basi bertahan setelah refresh | **fixed** `cb3cc46` — reset ke `-1` (konvensi constructor), tanpa memanggil `change_cb` karena `clear()` bukan aksi user. `TreeView::clear()` sengaja **tidak** ditambahkan: belum ada pemanggil yang butuh |
| `PromptDialog::input_at()` memetakan klik pakai `x + 18` sedangkan `draw()` menggambar kolom input di `x + 22` → caret meleset ~setengah karakter | **fixed** `1a4122f` — satu konstanta `PromptDialog::INPUT_PAD_X` dipakai `draw()` (teks + caret) dan `input_at()` |
| `Menu::relayout()` mengubah `w`/`h` tanpa menandai bounds lama **dan** baru sebagai dirty → ghosting saat menu mengecil | **fixed** `bc8112b` — `mark_area()` untuk bounds lama sebelum ubah ukuran + bounds baru sesudahnya (`mark_area()` sudah meng-union rect) |

Masih terbuka:

- `PromptDialog::on_click()` membatasi area klik kolom input ke `x + 12 … x + w - 12`,
  sedangkan kotak inputnya digambar di `x + 16 … x + w - 17` → klik di luar kotak
  masih memindahkan kursor. Ketemu saat menggarap offset teks, tidak disentuh
  karena di luar scope ticket itu.
- `Dialog::draw()` memakai `const_cast<char*>(text)[i] = '\0'` padahal `text`
  sudah `char*` (const_cast tidak perlu), dan memutasi state saat menggambar.
- Komentar `// Out-of-class PromptDialog (butuh Window lengkap).` di file lama
  berada di atas `Widget::set_visible`, bukan di atas definisi `PromptDialog`.

Regresi empat fix di atas dikunci di `test/libui_theme_test.cpp` (section 6–9).
Tiap fix diverifikasi dengan menjalankan test yang sama terhadap kode LAMA
(gagal / SIGFPE) dan kode baru (pass) — bukan cuma "test hijau".

> Catatan build: rule `test-libui-theme` di Makefile hanya mendaftarkan file
> `.cpp` sebagai dependency, jadi mengubah header saja **tidak** memicu rebuild
> test — `touch test/libui_theme_test.cpp` dulu sebelum menyimpulkan hasilnya.
