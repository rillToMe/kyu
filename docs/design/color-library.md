# Desain: Library Warna Modular `libs/color/`

> **Status**: SELESAI (2026-09-19) — library + wiring build + host test +
> migrasi `kernel/gfx/compositor.c`, `apps/libgui.c` (ABI gambar),
> konstanta dekorasi KWM (`kernel/gfx/kwm_internal.h`), toolkit `libs/widget/`
> (`Theme`/`Painter`/`aa_shade`), ABI tema `ui_theme_t`, dan tema 4 app
> userspace.
> **Verifikasi**: `make test-color`, `make` (kernel), `make apps`, dan
> screenshot QEMU pra/sesudah migrasi yang **identik byte-per-byte**.

## Tujuan

Format warna manual (`0xAARRGGBB` literal, `aa_mix`/`aa_shade` di
`include/aa_math.h`) tersebar di compositor, toolkit, dan app. Modul ini
menyediakan satu tipe nilai (`color_t`) + operasi blending, konversi ruang
warna, dan utility UI yang dipakai bersama kernel dan userspace — tanpa
alokasi heap dan tanpa float/SSE.

## Peta file

| File | Isi |
|------|-----|
| `include/color_types.h` | `color_t`, `color_format_t`, `COLOR_RGB`/`COLOR_RGBA`, `COLOR_RGB_INIT`/`COLOR_RGBA_INIT` (initializer constexpr), `color_to_u32`/`color_from_u32`, `color_with_alpha`/`color_opaque` (inline) |
| `include/color_blend.h` | `color_blend_alpha` (inline), `color_div255`, seam batch `color_blend_span` |
| `src/color_blend.c` | Batch span; `COLOR_BLEND_USE_SIMD` = hook SIMD (SSE2/AVX) |
| `include/color_space.h` | `color_hsl_t`/`color_hsv_t` + 4 fungsi konversi |
| `src/color_space.c` | Konversi RGB <-> HSL/HSV, integer + pembulatan |
| `include/color_utils.h` | Palet preset (+ bentuk initializer `COLOR_WHITE_INIT`/`COLOR_BLACK_INIT`/`COLOR_TRANSPARENT_INIT`), `color_darken`/`color_lighten`, `color_get_contrast_text` |
| `src/color_utils.c` | Implementasi utility UI |

Tidak ada `libs/color/Makefile` sendiri: sumber ikut build system yang ada
(kernel `SRC_DIRS` glob + `user_apps/Makefile` eksplisit) — lihat bagian Build.

## Keputusan desain

| Keputusan | Alasan |
|-----------|--------|
| `color_t` = RGBA 8-bit non-premultiplied, `a=0` transparan | Selaras model mask `display.h` (XRGB8888: byte tinggi 0 = transparan, non-zero = opaque) |
| Integer murni, tanpa float/SSE | Kernel & app dibangun `-mno-sse -mno-sse2 -msoft-float` (pola yang sama dengan `include/aa_math.h`) |
| Zero-allocation | Semua fungsi menerima/mengembalikan nilai `color_t`; tidak ada `malloc`/`kmalloc` di mana pun |
| `FORMAT_ARGB` (default) = `0xAARRGGBB` | Sama dengan tipe warna compositor; RGBA/ABGR/BGRA untuk variasi framebuffer hardware |
| `COLOR_RGB_INIT`/`*_INIT` = **constant expression**, bukan pemanggilan inline | Tema ditulis sebagai tabel `static const color_t`; `COLOR_RGB()`/`color_make()` adalah panggilan inline sehingga tidak sah sebagai initializer C (`initializer element is not a compile-time constant`). Nilainya identik dengan `COLOR_RGB(r, g, b)` (di-assert di `test/color_test.c`) |
| HSL/HSV: hue 0..359 derajat, s/l/v 0..255 | Picker UI menampilkan derajat; hindari fixed-point yang membingungkan di API |
| Header C/C++ compatible | toolkit `libs/widget/` (C++17; dulu `apps/libui.cpp`) memakai header yang sama; makro `COLOR_RGB`/`COLOR_RGBA` memanggil fungsi inline, bukan compound literal C |
| Palet `static const` di header | Tiap TU dapat salinan sendiri (tanpa storage global bersama), bebas warning di C & C++ |

## Kontrak blending

`color_blend_alpha(src, dst)` — overlay non-premultiplied, a = cakupan:

- `src.a == 0` → `dst` apa adanya (tidak menggambar).
- `dst.a == 0` → `src` dengan alpha dipaksa 255 (kanvas kosong).
- selain itu → `(s*src.a + d*(255-src.a)) / 255` per kanal dengan hasil
  **opaque (a=255)**, sesuai model display KyuzenOS yang hanya mengenal
  transparan/opaque.

> **Konsekuensi yang mudah menggigit**: `dst.a == 0` berarti "kanvas kosong",
> jadi warna apa pun yang dipakai sebagai **dst** (mis. `top` di
> `rrect_grad`/`vgrad`) **harus opaque**. Warna dari ABI app datang sebagai
> XRGB (byte alpha 0, lihat `libui.h`), jadi Theme menormalkannya lewat
> `color_opaque(color_from_u32(...))`. Tanpa itu gradien tombol rata dengan
> warna bawahnya — lihat `test/libui_theme_test.cpp`.

Formula itu **persis sama dengan `aa_mix()`** lama. Invarian ini dikunci
`test_aa_mix_parity` (semua cakupan 0..255 × 16 pasangan warna), supaya
pemakaian di compositor/libgui tidak menggeser satu piksel pun.

`color_div255()` (tanpa instruksi divide, presisi `x < 65536`) tetap dipakai
utility (darken/lighten) dan diuji eksak terhadap `x / 255`.

## Batas galat ruang warna

Round-trip RGB → HSL/HSV → RGB **maksimum ±3 per kanal**, akibat kuantisasi
hue 1 derajat, s/l/v 8-bit, dan pembulatan interpolasi. Diuji tidak hanya pada
sampel: `test/color_test.c` menyapu seluruh 256³ warna dan meng-assert batas
ini, jadi regresi rumus konversi langsung ketahuan.

## Migrasi compositor + libgui

- `kernel/gfx/compositor.c`: kanvas<->`color_t` lewat `color_from_u32`/
  `color_to_u32` (kanvas XRGB diperlakukan opaque sebelum blend), semua blend
  (shadow, tepi frame, sudut AA, gradien titlebar) lewat `color_blend_alpha`,
  warna kursor HW memakai palet `COLOR_WHITE`/`COLOR_BLACK`/`COLOR_TRANSPARENT`.
  `aa_math.h` tinggal dipakai untuk `aa_cov` (coverage geometri).
- `kernel/gfx/kwm_internal.h`: konstanta dekorasi jadi `static const color_t`.
- `apps/libgui.c`: helper internal memakai `color_t`; serialisasi pixel lewat
  `color_to_u32` (alpha tetap dipaksa opaque seperti `color | 0xFF000000` dulu).

### ABI gambar libgui = `color_t`

Setelah compositor + libgui selesai dimigrasikan, parameter warna API gambar
`libgui.h` ikut diubah dari `uint32_t` ke `color_t` — jadi tidak ada lagi
konversi hex di batas fungsi dan pemanggil menulis warna per komponen:

| Fungsi | Parameter |
|--------|-----------|
| `gui_draw_rect`, `gui_draw_char`, `gui_draw_text`, `gui_draw_label_num` | `color_t color` |
| `gui_draw_bar` | `color_t bar_color` |

- `gui_window_t.canvas` tetap `uint32_t*`: itu **memori pixel** (framebuffer
  window), bukan nilai warna — konversi `color_to_u32()` terjadi sekali di
  `_lgui_px()` dalam `apps/libgui.c`.
- Canvas libgui selalu opaque (alpha dipaksa 255 seperti sebelumnya), jadi
  memakai `COLOR_RGBA(..., 0)` pun tetap menggambar. Mask transparansi window
  urusan compositor, bukan app.
- Pemanggil yang ikut disesuaikan: toolkit widget `libs/widget/` (dulu `apps/libui.cpp`; helper `argb()` dihapus —
  tidak perlu lagi membungkus pixel), `user_apps/desktop.c` (14 konstanta warna
  jadi `COLOR_RGB(...)`, `parse_color()` mengembalikan `color_t` opaque,
  checksum daftar app lewat helper `rgb24()`), dan stub libgui di
  `test/textedit_test.cpp` + `test/libui_theme_test.cpp` +
  `test/desktop_manifest_test.c`.

Verifikasi: build HEAD (stash) vs build sekarang dijalankan di sesi yang sama,
9 dari 11 frame probe **identik byte-per-byte** (desktop c0–c3, notepad s1–s5);
2 sisa hanya berbeda pada teks jam kernel yang digambar live
(`kernel/timer_callbacks.c`) — baca selengkapnya di bagian Verifikasi.

## Migrasi tema app userspace

Empat app yang mendefinisikan tema sendiri kini menuliskan warnanya per
komponen lewat `COLOR_RGB_INIT()` (+ `COLOR_WHITE_INIT` dari palet) sebagai
tabel `static const color_t`, bukan lagi hex `0xAARRGGBB`:

| App | Tema |
|-----|------|
| `user_apps/notepad.c` | `NOTEPAD_THEME` (Modern Dark) |
| `user_apps/settings.c` | Preset Gelap/Terang/Hijau |
| `user_apps/widget_demo.c` | Preset yang sama + tema default di `main()` |
| `user_apps/terminal.c` | Tema terminal + `#define COLOR_PROMPT` (satu definisi untuk accent tema **dan** gaya prompt TextEdit, supaya tidak bisa menyimpang) |

**ABI `ui_theme_t` juga ikut pindah ke `color_t`** (lihat bagian berikutnya) —
jadi tabel `color_t` app bisa langsung di-`ui_window_set_theme()` tanpa
konversi. Byte alpha diisi `0xFF` dan diabaikan painter (permukaan window
memang opaque) — diverifikasi tidak menggeser satu piksel pun (bagian
Verifikasi).

## Migrasi ABI `ui_theme_t` + format file `settings.ui`

`ui_theme_t` sekarang 6 × `color_t` (dulu 6 × `uint32_t` `0x00RRGGBB`), dan
`ui_textedit_set_prompt_style()` menerima `color_t`. Karena ini mengubah ABI
sekaligus format file di disk, ada dua hal yang dijaga:

- **`Theme::set()`** menormalkan warna masuk lewat `color_opaque()`: pemanggil
  lama yang masih mengirim warna tanpa alpha tetap diperlakukan opaque (peran
  ini sebelumnya dipegang `argb()`/`keyboard` jalur `| 0xFF000000`).
- **Format `settings.ui` diberi versi** (lihat blok doc di `include/libui.h`):

| Versi | Layout | Ukuran | Status |
|-------|--------|--------|--------|
| v1 | tag `"KTH1"` + 6 × `color_t` (r,g,b,a) | 28 byte | yang ditulis sekarang |
| v0 | 6 × `uint32` `0x00RRGGBB` tanpa tag | 24 byte | file lama, **tetap dibaca** |

Versi dibedakan dari ukuran + tag, jadi tidak perlu menyentuh struct ABI.
Load tetap menolak blob bukan-theme (ukuran tak dikenal, tag salah, atau semua
RGB nol) dan mengembalikan 0 — app yang mengandalkan sinyal itu (`settings.c`,
`widget_demo.c`) tidak berubah perilakunya. Dokumentasi format ikut ditulis di
blok komentar `ui_settings_save`/`load` pada `include/libui.h` agar kontraknya
terlihat dari header, bukan hanya dari implementasi.

> Catatan migrasi file: file `settings.ui` v0 yang sudah ada di disk pengguna
> tetap dimuat lewat jalur lama, lalu ditulis ulang sebagai v1 saat `Simpan`
> berikutnya — tidak ada langkah manual yang diperlukan.

## Migrasi toolkit widget (`libs/widget/`; dulu `libui.cpp`) — Theme, Painter, `aa_shade`

- `Theme`: 6 warna ABI + 12 lapisan turunan jadi `color_t`. Warna ABI
  dinormalkan `color_opaque()` (XRGB → opaque) dan `to_abi()` mem-pack balik
  6 × `color_t` untuk `settings.ui` (v1); byte alpha file jadi 0xFF, dibaca
  ulang oleh `settings_load()` yang mengabaikan alpha.
- `Painter`: `rect`/`text`/`rrect*`/`vgrad` menerima `color_t`; `argb()`
  (force opaque) menggantikan `c | 0xFF000000`, `blend()` memakai
  `color_blend_alpha` di atas `color_opaque(color_from_u32(canvas))`.
- `aa_shade` → `color_darken`/`color_lighten` dengan konstanta `SHADE_*`
  (skala 0..255). `aa_shade` memakai persen + truncate sedangkan library
  memakai skala 255 + pembulatan, jadi faktor dipilih supaya hasilnya **sama
  persis** untuk palet charcoal tema (`#1E1E1E` button_bg, `#D4D4D4` fg) —
  selisih maksimum 1 unit pada warna lain. `aa_math.h` di libui kini tinggal
  `aa_cov` (coverage sudut).
- **ABI publik `libui.h` berubah** di dua titik: `ui_theme_t` 6 × `color_t`
  dan `ui_textedit_set_prompt_style(..., color_t color)`. Semua pemanggil di
  repo (`user_apps/notepad.c`, `settings.c`, `widget_demo.c`, `terminal.c`)
  ikut disesuaikan; app Rust/Zig di luar repo perlu menyesuaikan struct tema.
  File `settings.ui` lama tetap dibaca (lihat bagian format file).

Byte tinggi kanvas (mask transparansi) tetap 0 seperti sebelumnya: hasil blend
oleh library dipangkas `& 0x00FFFFFF`, jadi isi buffer identik di memori,
bukan cuma di layar.

## Build

| Jalur | Wiring |
|-------|--------|
| Kernel | `SRC_DIRS += libs/color/src`, `CFLAGS += -Ilibs/color/include` (top-level `Makefile`) |
| User apps | `CFLAGS_COMMON += -I../libs/color/include`; objek `$(COLOR_OBJS)` + rule kompilasi di `user_apps/Makefile` — app yang memakai fungsi out-of-line menambahkan `$(COLOR_OBJS)` ke baris link-nya |
| Host test warna | `make test-color` — mengompilasi sumber library asli di host dan mengecek header sebagai C++17 (`test/color_cxx_check.cpp`) |
| Host test libui | `make test-libui-theme` — sumber `libs/widget/` di-*link* (header per-layer di-*include*) supaya Theme/Button bisa diperiksa; render sungguhan dicek piksel-per-piksel |
| Host test desktop | `make test-desktop` — `user_apps/desktop.c` di-*include*; kini memeriksa `parse_color()` mengembalikan `color_t` opaque |

`test/color_test.c` dikecualikan dari `C_SOURCES` (host test, bukan task
kernel) — sama seperti host test lain yang ikut ter-glob saat
`make conc`/`make heap-stress`.

## Hook SIMD

`color_blend_span()` adalah satu-satunya seam batch: implementasi SSE2/AVX
cukup menyediakan `color_blend_span_simd()` dan mengaktifkan
`-DCOLOR_BLEND_USE_SIMD`; pemanggil tidak berubah. Build saat ini tetap scalar
karena kernel melarang SSE.

## Verifikasi

- `make test-color`: tipe/serialisasi 4 format, paritas `aa_mix` (semua
  cakupan), blend span, HSL/HSV (sweep 256³), darken/lighten, auto-contrast,
  palet — plus header dicek sebagai C++17.
- `make test-libui-theme`: tema ABI jadi opaque, `to_abi()` round-trip, dan
  **tiap baris gradien tombol == `aa_shade`/`aa_mix` jalur lama** (dicek
  piksel hasil render, bukan hanya rumus). Saat normalkan-opaque sengaja
  dibatalkan, test ini gagal — jadi regresinya benar-benar terkunci.
- `make` + `make apps`: seluruh kernel/app link tanpa warning pada sumber yang
disentuh (`-Wall -Wextra`), termasuk `make test-textedit` (48 PASS).
- Nilai tema tiap app dibandingkan literal-per-literal dengan versi sebelumnya
  (script pemeriksa sekali jalan: urutan & nilai RGB identik untuk 6–18 warna
  per app), dan QEMU before/after untuk `settings` + `widget_demo` + `terminal`
  menghasilkan frame **identik byte-per-byte** (`settings` hanya beda di area
  jam/waktu).
- **ABI libgui `color_t`**: build HEAD dan build baru dijalankan di sesi QEMU
  yang sama (benchmark adil — probe ini punya frame yang memang
  nondeterministik). Hasil: `c0`–`c3`, `s1`–`s5` (9 frame, termasuk desktop
  yang digambar `user_apps/desktop.c` lewat libgui) **identik byte-per-byte**.
  Dua frame sisanya hanya beda pada teks jam kernel yang digambar live oleh
  `kernel/timer_callbacks.c` (strip hijau `#00FF00` / kuning `#FFFF00` di
  kanan-atas): dalam satu build pun frame itu ada/tiada antar-run. Cara
  memisahkannya dari regresi: jalankan probe dua kali pada build yang sama —
  frame yang beda run-to-run adalah noise jam, bukan regresi.
- **Bukti visual**: tiga probe QEMU (`test/_menu_probe.py` c0–c3,
  `test/_dialog_probe.py` d1–d2, `test/_ui_probe.py all` s1–s5) dibandingkan
  byte-per-byte dengan capture pra-migrasi. Frame yang memuat tombol bergradien
  (s3 `bar cari`, s4, s5) **identik** — inilah bukti migrasi warna libui tidak
  menggeser satu piksel pun. `d1` hanya berbeda di area jam (nilai waktu), dan
  c0/c1/d2 kadang tertangkap sebelum compositor menggambar penuh (artefak
  timing probe, arahnya bisa bolak-balik antar run).
