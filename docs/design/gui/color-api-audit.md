# GUI Color API Audit

> **Catatan (pasca-migrasi CMake).** Dokumen ini ditulis saat proyek masih
> dibangun dengan Makefile, jadi perintah `make ...` di dalamnya merujuk build
> lama. Makefile sudah dihapus; padanan CMake-nya ada di
> [Building](../../development/building.md). Hasil verifikasi yang tercatat di sini
> sengaja tidak diubah — itu catatan apa yang benar-benar dijalankan saat itu.


> Status: AUDIT SAJA — tanpa perubahan kode. Dipicu rencana API readable (`#1E1E1E`-style).
> Scope: `libs/gui/color/` + seluruh pemakaian repo-wide. Diverifikasi dari implementasi aktual, bukan nama macro.

## Current Architecture

Lokasi aktual: `libs/gui/color/` (bukan `gui/color/`). 4 header + 3 source, tanpa Makefile sendiri:

| File | Isi |
|------|-----|
| `include/color_types.h` | `color_t`, `color_format_t`, `color_make`, `COLOR_RGB`/`COLOR_RGBA`, `COLOR_RGB_INIT`/`COLOR_RGBA_INIT`, `color_with_alpha`/`color_opaque`, `color_to_u32`/`color_from_u32` |
| `include/color_blend.h` | `color_div255`, `color_blend_alpha` (inline), `color_blend_span` (decl) |
| `src/color_blend.c` | `color_blend_span` loop scalar; seam `COLOR_BLEND_USE_SIMD` |
| `include/color_space.h` | `color_hsl_t`/`color_hsv_t` + 4 fungsi konversi |
| `src/color_space.c` | konversi integer-only (static helpers: `channel_max/min`, `div_round`, `hue_from_rgb`, `hue_channel`) |
| `include/color_utils.h` | 10 `static const color_t` palet + 3 `*_INIT` macro, `color_darken`/`color_lighten`/`color_get_contrast_text` (decl) |
| `src/color_utils.c` | implementasi utils (`CONTRAST_LUMA_THRESHOLD 128`, luma Rec.601 integer) |

Tidak ada parser/converter string hex. Tidak ada `COLOR_HEX` / `COLOR("...")` saat ini (grep repo-wide: nol hasil di luar `third_party/`).

Dependency map aktual (koreksi dari diagram usulan):

```text
libs/gui/color/            ← zero deps (hanya <stdint.h>)
  ↑           ↑          ↑           ↑
kernel/gfx/   libs/core/  libs/text/  libs/gui/widget/
(compositor,  (libgui)    (kzfont)    (Theme/Painter/ABI libui)
 kwm_internal)
  ↑           ↑
apps/*        libs/gui/libdesktop (ADAPTER, bukan dependen langsung:
              canvas.cpp::to_gui_color memetakan field per field)
system/desktop  ← TIDAK pakai gui/color; tipe sendiri
              (kyuzen::desktop::Color + rgb() di geometry.hpp,
               ~30 consts di system/desktop/theme.hpp)
raw-u32 islands (bypass total): drivers/tty.c, kernel/gfx/fb.c,
              kernel/panic/*, kernel/kernel.c boot markers,
              kernel/timer_callbacks.c, PNG/media (ARGB8888 mentah)
```

Dua divergensi penting: (1) desktop menduplikasi konsep warna (`Color{r,g,b,a}` + `rgb()`) alih-alih include `color_types.h`; (2) jalur TTY/fb/panic/boot memakai `uint32_t 0xRRGGBB` mentah ke `draw_rect/draw_char/draw_string` versi kernel (bukan `gui_draw_*`).

Build wiring: kernel via `SRC_DIRS += libs/gui/color/src` + `-Ilibs/gui/color/include` (top Makefile); user apps via `COLOR_SRCS`/`COLOR_OBJS` di `apps/Makefile` (`LIBUI_COLOR_OBJS` hanya `color_utils.o` untuk sebagian target).

## Current Color Representation

Terverifikasi dari `color_types.h`:

* Tipe: `struct { uint8_t r, g, b, a; } color_t` — 4 byte, urutan field R,G,B,A.
* Model: RGBA 8-bit per kanal, **non-premultiplied, straight alpha**. `a=0` = transparan, `a!=0` = opaque (kontrak `display.h`: byte tinggi = mask 1-bit, bukan alpha bertingkat).
* Bukan XRGB/ARGB di memori — itu hanya bentuk serialisasi. `color_to_u32(c, FORMAT_ARGB)` = `0xAARRGGBB`; `FORMAT_RGBA`/`ABGR`/`BGRA` untuk varian hardware. Default = `FORMAT_ARGB`.
* Endian: struct field-based → tidak ada asumsi endian. Hanya serialisasi `uint32_t` (shift-based, endian-independent sebagai nilai).
* Framebuffer: `DISPLAY_FMT_XRGB8888`, low 24 bit = `0xRRGGBB`, high byte = mask (`display.h`). **Tidak cocok langsung** — konversi di tiap batas:
  * compositor: `xrgb_px()` = `color_to_u32(c, FORMAT_ARGB) & 0x00FFFFFFu`; baca-balik via `color_opaque(color_from_u32(px, FORMAT_ARGB))`.
  * libgui: `_lgui_px()` = paksa `a=255` lalu `color_to_u32(..., FORMAT_ARGB)`.
  * painter `blend()`, `kz_text_draw()`: pola sama (`color_with_alpha` untuk coverage → `color_blend_alpha` di atas `color_opaque(color_from_u32(...))`).
* Premultiplied: tidak — kecuali subsistem Rust terpisah (`PremultipliedRgbaColor` di `rust/kyuzen-gui`, di luar scope C API ini).
* Dua `COLOR_RGB` vs `COLOR_RGB_INIT`: `COLOR_RGB()` = pemanggilan `color_make()` inline → **bukan constant expression C** → ilegal di initializer `static const`. `COLOR_RGB_INIT()` = braced list `{r,g,b,255}` → constant expression → untuk tabel statik (`ui_theme_t`, tema app). Nilai identik (dulu di-assert `color_test.c` — lihat Risks).

## Current Public API

Dari header (lengkap, tanpa tebakan):

* Tipe/enum: `color_t`, `color_format_t {FORMAT_ARGB, FORMAT_RGBA, FORMAT_ABGR, FORMAT_BGRA}`, `color_hsl_t {h 0..359, s, l}`, `color_hsv_t {h, s, v}`.
* Konstruktor: `color_make(r,g,b,a)` (inline), `COLOR_RGB(r,g,b)`, `COLOR_RGBA(r,g,b,a)`, `COLOR_RGB_INIT(r,g,b)`, `COLOR_RGBA_INIT(r,g,b,a)`.
* Adapter: `color_with_alpha`, `color_opaque`, `color_to_u32`, `color_from_u32`.
* Blend: `color_blend_alpha` (inline, parity `aa_mix`), `color_div255`, `color_blend_span` (+ seam `color_blend_span_simd` bila `COLOR_BLEND_USE_SIMD`).
* Space: `color_rgb_to_hsl/hsl_to_rgb/rgb_to_hsv/hsv_to_rgb` (integer, round-trip ±3/kanal).
* Utils: `color_darken/lighten` (skala 0..255), `color_get_contrast_text` (luma Rec.601, threshold 128).
* Palet: `COLOR_TRANSPARENT/BLACK/WHITE/RED/GREEN/BLUE/YELLOW/CYAN/MAGENTA/GRAY` (`static const` per-TU) + `COLOR_TRANSPARENT_INIT/COLOR_BLACK_INIT/COLOR_WHITE_INIT`.
* Internal (bukan API): `channel_max/min`, `div_round`, `hue_from_rgb`, `hue_channel`, `CONTRAST_LUMA_THRESHOLD`, `xrgb_px` (compositor-static), `_lgui_px` (libgui-static), `to_gui_color` (canvas.cpp-static), `px_color` (desktop app_icons).

## Repository Usage

### A. Public API usage (apps / user / libui / libgui)

* `apps/notepad.c:57-63` — `NOTEPAD_THEME` 6× `COLOR_RGB_INIT` (`0x2D2D2D`, `0xD4D4D4`, `0x0098BC`, `0x1E1E1E`, `0xD4D4D4`, `0x3E3E42`). Inilah contoh target `#1E1E1E/#FFFFFF/#0098BC/#3E3E42` di request.
* `apps/terminal.c:17,141-146` — `COLOR_PROMPT COLOR_RGB(0x7C,0xC7,0xFF)` + tema 6× `COLOR_RGB`.
* `apps/widget_demo.c:138-150,222-227` — 4 preset `COLOR_RGB_INIT` + assignment runtime `COLOR_RGB`/`COLOR_WHITE`.
* `apps/settings/appearance.cpp:9-21` — 4 preset sama; `apps/settings/fonts.cpp:37`, `apps/fontdemo.c:125` — `COLOR_RGB(0xE8,0xE8,0xEC)`.
* `libs/gui/widget/include/core/theme.hpp:28-30,61-71` — default `Theme()` 6× `COLOR_RGB` + 8 derived surfaces hardcoded (`0x25,0x25,0x26`, `0x2D2D2D`, `0x3C3C3C`, `0x333333`, `0x454545`, `0xDCDCAA`, `0x00E5FF`); `set()` normalisasi `color_opaque()`.
* `libs/gui/widget` lain: `textedit.hpp:54` prompt default `COLOR_RGB(0x7C,0xC7,0xFF)`; `button.hpp:33-35`, `painter.hpp:194-197,125-138` `COLOR_BLACK` + blend/gradien; `window.hpp:31` pack ABI.
* `libs/core/libgui.c:159,203,335` — `COLOR_RGB(0xF5F5F5)` (bg awal), `(0x1E293B)` (desktop bg), `(0x333333)` (bar); `_lgui_px` satu-satunya serialisasi pixel.
* `libs/text/src/kzfont.c:273-308` (`kz_text_draw`) — coverage×alpha → `color_blend_alpha`.
* `libs/gui/libdesktop/src/canvas.cpp:22-29` — `to_gui_color` (`a ? a : 255`).
* `system/desktop/launcher.cpp:65-89` — `parse_color()` runtime (`0x...`/desimal → `rgb()` opaque). `theme.hpp` ~30 `rgb(0x..)` consts. `launcher.cpp:733-734` struct-literal mentah `{r,g,b,255}`, `{0,0,0,90}`.

### B. Internal kernel usage

* `kernel/gfx/compositor.c` — `fill_rect_clip`, `blend_px`, `blend_rect_clip`, `round_grad_fill`, bayangan/tepi/AA/titlebar; `COLOR_BLACK/WHITE/TRANSPARENT` untuk kursor HW (`:595-597,833-834`).
* `kernel/gfx/kwm_internal.h:24-29,43` — 7 `static const color_t` literal mentah (`{0x2D..}`, `{0xE81123..}`, edge `{0x9A..}` + `KWM_EDGE_ALPHA 70`).

### C. Test/harness usage

* `tests/host/unit/text_test.c`, `text_ft_real.c` — `COLOR_RGB(0xFF,0xFF,0xFF)`.
* `tests/host/unit/libui_theme_test.cpp:174-179,186-187,275-287` — tema notepad 6 warna + round-trip `settings.ui` v1/v0 + normalisasi alpha-0.
* `tests/host/unit/libui_owner_test.cpp`, `libui_fileman_widgets_test.cpp`, `desktop_manifest_test.cpp:201-210` — stub `gui_draw_*` dengan `color_t` + `color_to_u32(color_opaque(c), FORMAT_ARGB)`.
* `desktop_manifest_test.cpp:339-357` — `parse_color("0x1565C0")`, `rgb(...)`, `APP_DEFAULT`.
* **Anomali**: `Makefile:383-388` (`make test-color`) + `docs/design/color-library.md` merujuk `tests/host/unit/color_test.c` (+ `color_cxx_check.cpp`) — **kedua file tidak ada di disk maupun di git**. Klaim verifikasi (paritas `aa_mix`, sweep 256³, assert INIT==runtime) saat ini tidak bisa dieksekusi.

### D. Hardcoded colors

* `drivers/tty.c:26-27` — `#define BG_COLOR 0x1E1E1E`, `FG_COLOR 0xFFFFFF` (jalur `draw_rect/draw_char` u32 mentah).
* `kernel/timer_callbacks.c:64-65,87-88` — `0x1E1E1E`, `0xFFFF00`, `0x00FF00` (jam/spinner live).
* `kernel/kernel.c:100-104` — `BOOT_C_OK 0x7DDB8A`, `FAIL 0xFF6B6B`, `WARN 0xFFCC66`, `INFO 0x7CC7FF`, `TEXT 0xFFFFFF` via `tty_set_fg`.
* `kernel/panic/panic_internal.h:46-52` — `C_BG 0x001144`, `C_FG 0xFFFFFF`, `C_TITLE 0xFF4444`, `C_KEY 0xFFCC00`, `C_DIM 0x88AAFF`, `C_FAINT 0x6E82A8`, `C_RULE 0x3A4E78`, `C_OK 0x44FF44`.
* `kernel/gfx/fb.c:177-218` — `draw_rect/char/string/pixel/image` u32 mentah (`pixel & 0xFFFFFF`, `alpha>0`).
* `libs/gui/widget/include/core/painter.hpp:101,110`, `canvas.cpp:106,118`, `app_icons.hpp:blit_px` — blend ARGB mentah (`| 0xFF000000u`, `(sr*a+dr*na)/255u`) duplikat rumus `color_blend_alpha` tanpa memanggilnya.
* `rust/apps/*/ui/*.slint` — `#1565C0`, `#202020`, `#FFFFFF` dst. (Slint DSL, bukan C — bukti developer familiarity dengan `#RRGGBB`, tapi tidak bisa dicontoh ke C kernel).

## Hardcoded Color Audit

Ringkas: dua dunia paralel. Dunia `color_t` (compositor, libgui, widget, 4 app C, settings C++) vs dunia u32 mentah (TTY, fb, panic, boot, timer, PNG/media, desktop `Color`, painter/canvas fast-blit). Contoh `#1E1E1E` muncul di **kedua** dunia: `notepad.c` (`COLOR_RGB_INIT(0x1E,0x1E,0x1E)`) dan `tty.c` (`BG_COLOR 0x1E1E1E`) — nilai sama, representasi beda, tidak ada konverter bersama. Setiap opsi API baru harus memutuskan: (a) hanya gula sintaks di dunia `color_t`, atau (b) unifikasi juga dunia u32 (jauh lebih besar, sentuh panic path bebas-lock — tidak disarankan).

## ABI / ABI-like Compatibility

* `color_t` = 4×`uint8` = 4 byte, alignment 1. Ukuran sama dengan `uint32_t` lama tapi **layout field beda** — migrasi besar itu sudah terjadi sebelumnya (`ui_theme_t` 6×u32 → 6×`color_t`; `settings.ui` v0 24B → v1 tag `KTH1`+28B, loader tetap baca v0). Tidak ada perubahan ABI yang tertunda.
* Penambahan macro konstruktor murni (`COLOR_HEX` sebagai braced-list) = **layout-neutral, ABI-neutral**: tidak mengubah `sizeof`/`offset`, tidak menyentuh signature (`gui_draw_*`, `ui_theme_t`, `ui_textedit_set_prompt_style` tetap `color_t`), tidak menyentuh format file. Risiko ABI = nol bila implementasi sebagai macro initializer + inline setara (pola `COLOR_RGB_INIT` yang sudah ada).
* Yang MENGUBAH ABI (jangan dilakukan dalam fase gula-sintaks): mengganti `color_t` jadi `uint32_t`, menambah field, mengubah urutan field, mengubah `ui_theme_t`, mengubah `settings.ui`. Rust `kyuzen-gui` dan app eksternal (catatan di `color-library.md`: app Rust/Zig) mengasumsikan layout tema saat ini.
* `static const color_t` di header = salinan per-TU (bukan simbol global) → menambah konstanta palet tidak mengubah ABI.

## Performance Considerations

* Hot paths per-pixel: `compositor.c` (`blend_rect_clip`, `round_grad_fill`, `mix_content` + memcpy fast-path bila `fully_opaque`), `painter::blend/vgrad/image`, `kz_text_draw` (per coverage-pixel), `Canvas::draw_px`, `blit_px` RLE desktop. Semua sensitif terhadap **branch/divide**, bukan terhadap sintaks konstruktor.
* Konstruktor hex yang benar (`COLOR_HEX` sebagai shift+mask constant-expression) = **zero runtime overhead**: dilipat compiler menjadi 3 byte konstan, identik dengan `COLOR_RGB_INIT`. Tidak menambah instruksi di hot path.
* `COLOR("#..")` runtime-parse = overhead nyata (loop + branch per panggilan) + tidak bisa dipakai di `static const` → dilarang di hot path dan tidak menyelesaikan kasus utama (tabel tema statik).
* `color_blend_span` adalah satu-satunya seam batch/SIMD; sintaks konstruktor tidak menyentuhnya. Jaga: macro baru tidak boleh memaksa evaluasi argumen >1× (gunakan argumen sekali per channel atau dokumentasikan single-evaluation; pola existing `COLOR_RGB` juga multi-eval — konsisten saja).
* `fully_opaque` / base-blit elision / occlusion culling (kwm.c/compositor.c Phase 14/16-18/20) bergantung pada semantik opaque, bukan sintaks — tidak terdampak selama alpha default = 255.

## Target Developer Experience

`COLOR_RGB_INIT(0x1E, 0x1E, 0x1E)` vs `#1E1E1E`: keluhan sah — tiga byte terpisah mengulang `0x`, tidak bisa copy-paste dari spec/desainer, tidak grep-able sebagai satu token warna, urutan channel implisit.

## Option A — COLOR_HEX(0xRRGGBB)

```c
#define COLOR_HEX(v) { (uint8_t)(((v) >> 16) & 0xFF), (uint8_t)(((v) >> 8) & 0xFF), (uint8_t)((v) & 0xFF), 255u }
static const color_t C = COLOR_HEX(0x1E1E1E);   // + varian runtime COLOR_HEX_C(0x1E1E1E) via color_make
```

* Compile-time init: YA (integer constant expression bila argumen konstan) — syarat utama tabel tema terpenuhi.
* Zero overhead: YA (fold ke 3 byte).
* Freestanding/kernel/user: YA (hanya shift+mask; C dan C++ compatible, tanpa compound literal bila bentuk braced-list untuk init + bentuk `color_make` untuk ekspresi — cermin pola `COLOR_RGB`/`COLOR_RGB_INIT` yang sudah ada).
* Readability: `0x1E1E1E` ≈ `#1E1E1E` satu token, copy-paste dari spec (tambah `0x`, atau `HEX` macro menerima persis 6 digit).
* Trade-off: (1) ambiguitas urutan — harus dokumen keras `0xRRGGBB` (bukan `0xBBGGRR`), karena repo juga mengenal `0xAARRGGBB` di jalur mentah; (2) alpha butuh varian kedua (`COLOR_HEX_A(v)` untuk `0xAARRGGBB` atau tolak alpha di HEX); (3) multi-evaluasi argumen bila macro naive (pakai sekali per channel — 3× eval; terima seperti existing, atau sediakan inline `color_from_hex24(uint32_t)` untuk runtime); (4) migrasi mekanis tapi menyentuh banyak file bila diadopsi massal (untungnya binary-identical, verifikasi via screenshot QEMU byte-identical seperti migrasi sebelumnya).

## Option B — COLOR("#RRGGBB")

```c
color_t c = COLOR("#1E1E1E");   // atau color_from_hex("#1E1E1E")
```

* Tidak realistis sebagai pengganti konstruktor di C freestanding untuk kasus utama:
  * String literal bukan constant expression untuk struct init → **tabel `static const` (notepad/terminal/widget_demo/settings/theme) tidak bisa memakainya**. Itu 90% keluhan readability.
  * Runtime parse per panggilan (loop, validasi `#`, panjang 3/6/8, hex digit, case) = overhead non-nol; di hot path harus di-cache manual.
  * Semantik error baru: string invalid → warna apa? (hitam diam-diam? fallback? assert?) — menambah kontrak failure ke header zero-alloc.
* Realistis hanya sebagai **runtime helper** (`color_t color_from_hex(const char *s)` + mungkin `parse_color` desktop dimigrasi ke sana): berguna untuk manifest/config/input user, bukan untuk konstanta kode. Freestanding OK (hand-rolled, tanpa libc), kernel OK di luar panic path, user OK. compile-time safety = tidak ada (typo `"#1E1E1G"` lolos compile, gagal runtime).
* C++ `constexpr` parse bisa zero-overhead, tapi header ini shared C (kernel) — C tidak punya `constexpr` function — jadi tidak bisa jadi fondasi lintas kernel/user.

## Option C — Literal-style Approach

Target harfiah `#1E1E1E` sebagai sintaks ekspresi C/C++: **tidak valid, katakan jelas.**

* Di luar definisi macro, `#` memulai preprocessing directive (`#include`, `#define`...) atau — di dalam function-like macro — operator stringize (`#param`). `#1E1E1E` di posisi ekspresi = compile error di semua compiler C/C++ yang valid.
* Jalan buntu yang sudah diverifikasi tidak layak: digit-separator (`1E1E1E` bukan angka valid; `'` C++14 hanya antar-digit desimal), token-paste `##` (butuh dua token valid dulu), user-defined literal C++ (`0x1E1E1E_hex` — butuh `operator""`, `constexpr`, namespace; tidak ada di C kernel; toolchain freestanding + shared C/C++ header = friksi; tetap bukan `#...`).
* Hack buruk yang ditolak: `#define` satu nama per warna (`#define _1E1E1E ...` — identifier tidak boleh diawali digit; nama seperti `C_1E1E1E` = palet bernama, bukan literal), codegen script (build complexity untuk nol gain semantik), komentar-magic.
* Alternatif paling dekat yang valid: **Option A** (`COLOR_HEX(0x1E1E1E)` — beda 9 karakter dari `#1E1E1E`) + palet bernama untuk warna kanonis (`COLOR_BG_DARK` dst. bila diinginkan). Slint sudah membuktikan `#RRGGBB` familiar — tapi itu DSL, bukan C.

## Recommended Direction

Tanpa implementasi — arah yang didukung audit:

1. **Adopsi Option A** (`COLOR_HEX(0xRRGGBB)` + `COLOR_HEX_A(0xAARRGGBB)` bila perlu) sebagai gula di `color_types.h`, cermin pola `RGB`/`RGB_INIT` ganda (init-list + inline-ekspresi). Satu-satunya opsi yang memenuhi semua constraint: compile-time init, zero overhead, freestanding, kernel+user, C+C++.
2. **Tambahkan `color_from_hex()` runtime HANYA bila ada konsumen** (unifikasi `parse_color` desktop) — bukan sebagai konstruktor utama. Jangan campur kedua peran dalam satu macro.
3. **Tolak Option B sebagai konstruktor dan Option C mentah** dengan alasan di atas (catat di doc agar tidak diusulkan ulang).
4. **Semantic layer tetap di atas `gui/color/`**: `ui_theme_t` 6 field + `Theme::derive()` 8 surfaces (widget), `theme.hpp` desktop. Jangan tambah `COLOR_WINDOW_BG` ke color headers — raw vs semantic harus tetap terpisah (bagian 6).
5. **Prasyarat sebelum implementasi**: pulihkan `tests/host/unit/color_test.c` (atau cabut referensinya dari Makefile/doc) agar klaim binary-identical bisa dieksekusi lagi; putuskan cakupan (hanya call-site `color_t` baru vs migrasi massal termasuk `kwm_internal.h` literal mentah vs dunia u32 mentah yang sebaiknya disentuh terpisah).

## Migration Impact

* Bila aditif murni (macro baru, call-site lama tidak disentuh): dampak nol — tidak ada file wajib berubah selain `libs/gui/color/include/color_types.h` (+ test).
* Bila migrasi sukarela per call-site: setiap `COLOR_RGB_INIT(a,b,c)` → `COLOR_HEX(0xAABBCC)` adalah transformasi 1:1 binary-identical; verifikasi per-file via `gcc -E`/diff + QEMU screenshot probe (pola migrasi `color-library.md` §Verifikasi).
* Bila unifikasi desktop `Color`/`rgb()` → `color_t`: keputusan arsitektur terpisah (sentuh `geometry.hpp`, `theme.hpp`, `canvas.cpp`, `launcher.cpp`, host test desktop) — di luar scope gula-sintaks, risiko lebih tinggi (init statis `.init_array` constraint desktop).
* Bila menyentuh dunia u32 (tty/fb/panic/boot): risiko tertinggi (panic path no-lock/no-alloc, TTY early-boot sebelum heap) — tidak direkomendasikan sefase.

## Files Likely Affected

Fase implementasi berikutnya (bila arah disetujui), urut risiko:

1. `libs/gui/color/include/color_types.h` — macro baru (wajib; satu-satunya file inti).
2. `tests/host/unit/color_test.c` — pulihkan/buat (assert HEX==INIT==runtime, semua format, paritas blend).
3. Call-site sukarela (binary-identical): `apps/notepad.c`, `apps/terminal.c`, `apps/widget_demo.c`, `apps/settings/appearance.cpp`, `libs/gui/widget/include/core/theme.hpp`, `kernel/gfx/kwm_internal.h` (literal mentah → INIT/HEX).
4. Opsional terpisah: `system/desktop/*` (unifikasi `Color`), `libs/gui/color/include/color_utils.h` (palet bernama semantik bila diinginkan — lebih baik di theme layer).
5. Jangan sentuh: `kernel/panic/*`, `drivers/tty.c`, `kernel/gfx/fb.c`, `kernel/kernel.c`, `kernel/timer_callbacks.c`, `libs/media/*`, `rust/*`, `settings.ui` format, `ui_theme_t`.

## Risks

* Ambiguitas `0xRRGGBB` vs `0xAARRGGBB` dalam satu codebase (jalur mentah memakai makna kedua) — mitigasi: nama macro eksplisit + doc + test urutan channel.
* Multi-evaluasi argumen macro (konsisten dengan existing, tapi dokumentasikan).
* Referensi test hilang (`color_test.c`/`color_cxx_check.cpp`): klaim parity/sweep saat ini unverifiable; `make test-color` gagal pada tree bersih — pulihkan dulu sebelum klaim binary-identical untuk migrasi baru.
* Scope creep ke semantic colors / unifikasi desktop / dunia u32 — tahan di fase terpisah.
* Slint `#RRGGBB` menimbulkan ekspektasi salah bahwa C bisa sama — doc ini menolaknya eksplisit.

## Conclusion

Representasi internal (`struct r,g,b,a` + serialisasi `FORMAT_ARGB` ke XRGB) sehat dan tidak perlu berubah. Masalahnya murni ergonomi konstruktor (3 argumen vs 1 token hex). `COLOR_HEX(0xRRGGBB)` adalah titik optimal: valid C/C++ constant expression, zero overhead, freestanding, ABI-neutral. `COLOR("#..")` hanya layak sebagai runtime helper config, bukan konstruktor. `#RRGGBB` literal tidak achievable di C — berhenti mengupayakannya. Semantic colors sudah ada (theme layers) dan harus tetap di atas `gui/color/`, bukan di dalamnya. Berhenti di sini; tunggu keputusan sebelum implementasi.
