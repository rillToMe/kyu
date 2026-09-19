# Desain: Library Warna Modular `libs/color/`

> **Status**: SELESAI (2026-09-19) — library + wiring build + host test.
> **Cakupan**: pembuatan library. Migrasi pemakaian hex manual
> (`0xAARRGGBB`) di compositor, libgui, libui, dan app adalah langkah
> terpisah dan **belum** dikerjakan di sini.
> **Verifikasi**: `make test-color`, `make` (kernel), `make apps`.

## Tujuan

Format warna manual (`0xAARRGGBB` literal, `aa_mix`/`aa_shade` di
`include/aa_math.h`) tersebar di compositor, toolkit, dan app. Modul ini
menyediakan satu tipe nilai (`color_t`) + operasi blending, konversi ruang
warna, dan utility UI yang dipakai bersama kernel dan userspace — tanpa
alokasi heap dan tanpa float/SSE.

## Peta file

| File | Isi |
|------|-----|
| `include/color_types.h` | `color_t`, `color_format_t`, `COLOR_RGB`/`COLOR_RGBA`, `color_to_u32`/`color_from_u32` (inline) |
| `include/color_blend.h` | `color_blend_alpha` (inline), `color_div255`, seam batch `color_blend_span` |
| `src/color_blend.c` | Batch span; `COLOR_BLEND_USE_SIMD` = hook SIMD (SSE2/AVX) |
| `include/color_space.h` | `color_hsl_t`/`color_hsv_t` + 4 fungsi konversi |
| `src/color_space.c` | Konversi RGB <-> HSL/HSV, integer + pembulatan |
| `include/color_utils.h` | Palet preset, `color_darken`/`color_lighten`, `color_get_contrast_text` |
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
| HSL/HSV: hue 0..359 derajat, s/l/v 0..255 | Picker UI menampilkan derajat; hindari fixed-point yang membingungkan di API |
| Header C/C++ compatible | `apps/libui.cpp` (C++17) memakai header yang sama; makro `COLOR_RGB`/`COLOR_RGBA` memanggil fungsi inline, bukan compound literal C |
| Palet `static const` di header | Tiap TU dapat salinan sendiri (tanpa storage global bersama), bebas warning di C & C++ |

## Kontrak blending

`color_blend_alpha(src, dst)` — overlay non-premultiplied, a = cakupan:

- `src.a == 0` → `dst` apa adanya (tidak menggambar).
- `dst.a == 0` → `src` dengan alpha dipaksa 255 (kanvas kosong).
- selain itu → campuran RGB per kanal dengan hasil **opaque (a=255)**,
  sesuai model display KyuzenOS yang hanya mengenal transparan/opaque.

Pembagian 255 memakai `color_div255()` (tanpa instruksi divide, presisi untuk
`x < 65536`); seluruh rentang produk kanal diuji eksak terhadap `x / 255`.

## Batas galat ruang warna

Round-trip RGB → HSL/HSV → RGB **maksimum ±3 per kanal**, akibat kuantisasi
hue 1 derajat, s/l/v 8-bit, dan pembulatan interpolasi. Diuji tidak hanya pada
sampel: `test/color_test.c` menyapu seluruh 256³ warna dan meng-assert batas
ini, jadi regresi rumus konversi langsung ketahuan.

## Build

| Jalur | Wiring |
|-------|--------|
| Kernel | `SRC_DIRS += libs/color/src`, `CFLAGS += -Ilibs/color/include` (top-level `Makefile`) |
| User apps | `CFLAGS_COMMON += -I../libs/color/include`; objek `$(COLOR_OBJS)` + rule kompilasi di `user_apps/Makefile` — app yang memakai fungsi out-of-line menambahkan `$(COLOR_OBJS)` ke baris link-nya |
| Host test | `make test-color` — mengompilasi sumber library asli di host dan mengecek header sebagai C++17 (`test/color_cxx_check.cpp`) |

`test/color_test.c` dikecualikan dari `C_SOURCES` (host test, bukan task
kernel) — sama seperti host test lain yang ikut ter-glob saat
`make conc`/`make heap-stress`.

## Hook SIMD

`color_blend_span()` adalah satu-satunya seam batch: implementasi SSE2/AVX
cukup menyediakan `color_blend_span_simd()` dan mengaktifkan
`-DCOLOR_BLEND_USE_SIMD`; pemanggil tidak berubah. Build saat ini tetap scalar
karena kernel melarang SSE.
