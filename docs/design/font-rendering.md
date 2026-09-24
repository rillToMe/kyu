# Font Rendering — FreeType Integration (libs/text)

Status: BRING-UP PASS (2026-09-22) — FreeType 2.14.3 freestanding
terkompilasi + terlink statis + merasterize DejaVu Sans 'A' 16px nyata
via kz API (`make test-text-ft`). Detail §2, §10, §12.

```text
UI (libgui/libui: gui_draw_text, Painter::text — bitmap 8x16, tetap)
 ↓  (integrasi AA: caller opsional — kz_text_draw + gui_damage_rect)
Kyuzen Font API (libs/text/include/kzfont.h — TANPA FT_*)
 ↓
Font Manager (libs/text/src/kzfont.c: heap inject, cache, layout)
 ↓
Raster backend (kz_raster_backend_t)
 ↓  KZFONT_USE_FREETYPE=1: FreeType (kzraster_ft.c)
 ↓  tanpa flag: MISS semua -> fallback bitmap (aman di-link tanpa FT)
Glyph Cache (open-address 256 slot, key (font, px, codepoint))
 ↓
Renderer existing (color_blend_alpha + canvas XRGB + gui_damage_rect)
 ↓
Compositor / GHAL (software · virtio-gpu · intel — tak tersentuh)
```

## 1. FreeType version

Vendored: `third_party/freetype/` = **FreeType 2.14.3** (sudah ada
sebelum task ini — Phase 1 PARTIAL, bukan fetch baru).
Lisensi: FreeType License (BSD-style) — kompatibel, atribusi di
`third_party/freetype/LICENSE.TXT`. Target modul: **truetype + sfnt +
smooth + psnames** saja (fragmen: `third_party/freetype/kyuzen.mk`).

## 2. Build configuration

* `libs/text` = userspace (ring 3), BUKAN kernel. Kernel/TTY/panic
  tetap bitmap `font8x16`; FT tidak masuk link kernel (object FT di
  var terpisah, bukan ALL_OBJS).
* Port Kyuzen TANPA patch upstream (`third_party/freetype/kyuzen/`):
  `include/ftkz_stdlib.h` (via `FT_CONFIG_STANDARD_LIBRARY_H` —
  string/sort/strtol/getenv/setjmp ke `kzf_*` di `src/kzf_port.c`),
  `include/ftkz_modules.h` (via `FT_CONFIG_MODULES_H` — 4 modul:
  truetype/sfnt/psnames/smooth), shim `include/string.h` +
  `include/setjmp.h` (untuk `#include` langsung md5.c/ftgrays.c),
  `src/kzf_system.c` (pengganti ftsystem.c: no-fopen, no-trace,
  no-global-allocator). Inventarisasi dependensi: hanya
  mem*/str*/qsort/strtol/getenv(jmp) yang dipakai set minimal;
  `double` hanya di FT_TRACE (terkompilasi hilang); ftgrays tidak
  memanggil setjmp (hanya include).
* `setjmp`/`longjmp` = `__builtin` clang. Probe empiris (host O0+O2):
  builtin menulis tepat 3 word (0..2); buffer 8 word. Semua call-site
  `ft_longjmp` upstream memakai literal 1 (grep) sehingga makro
  melompat dengan 1 — perilaku identik (semua call-site cek `== 0`).
* `ftsystem.c`/`ftdebug.c` hulu TIDAK dikompilasi. Simbol yang masih
  dirujuk disediakan jujur di `kzf_system.c`: `FT_Stream_Open` →
  `Cannot_Open_Resource` (hanya memory stream), `FT_Trace_*` → no-op
  (trace tak didukung), `FT_New_Memory` → NULL (pakai `FT_New_Library`
  + `kz_heap`, persis `kzraster_ft.c`), `FT_Gzip_Uncompress` → REAL
  (ftgzip.c dikompilasi; embedded zlib via `FT_Memory`).
* Flag FT == kebijakan userspace existing (`apps/Makefile`
  CFLAGS_LIB): freestanding, `-mno-sse*`, `-msoft-float`, `-O2`.
  BUKAN CFLAGS kernel (`-mcmodel=kernel` salah untuk userspace).
* Artefak: `build/lib/libfreetype_kyuzen.a` (14 TU, ~427KB).
  Bukti no-host-libc: closure simbol arsip = 681 defined,
  103 undefined yang semuanya internal, **0 external** (llvm-nm) —
  tanpa memcpy/malloc/float-helper global.

## 3. Allocator integration (Phase 2)

Tidak ada allocator kedua. `kz_heap_t` (alloc/free/realloc) di-inject
saat `kz_font_load`. App → `sys_alloc`/`sys_free`/`sys_realloc`
(uheap per-proses); host test → malloc. FT314N dipetakan lewat
`FT_MemoryRec_` (`kz_ft_alloc/free/realloc` di `kzraster_ft.c`).
Kernel `kmalloc` TIDAK dipakai (libtext userspace-only).

## 4. Font loading (Phase 3/4)

`kz_font_blob_t {data, size}` = **borrowed** (embedded asset milik
caller, hidup selama font hidup) → `FT_New_Memory_Face` (tanpa
`fopen`/VFS — audit: `sys_read_file_to_buffer` ada tapi tidak dipakai
tahap awal, sesuai instruksi). `kz_font_set_size(px)` 1..256.

## 5. Glyph cache (Phase 6/15)

Open-addressing 256 slot, key `(codepoint, px)`. Insert di slot bebas
chain; penuh → full-drop + insert ulang (selalu benar tanpa
tombstone; ceiling = miss spike sesekali, bukan LRU). Coverage milik
slot (pitch = width), hidup selama font hidup. Statistik:
`kz_font_stats` (lookups/hits/misses) — pola uji Phase 15:
render "Hello Kyuzen" 2x → 10 miss + 2 hit, lalu 12 hit.

## 6. Text API (Phase 5/8/9/10/11)

`kz_glyph_t`: left/top/width/height/advance + `coverage` 8-bit
(pitch = width). `kz_text_measure` (width = Σ advance, height =
asc−desc) dan `kz_text_draw(canvas, cw, ch, font, x, baseline_y,
color, text, dmg[4])`. Draw = coverage × alpha → `color_blend_alpha`
**existing** (tanpa blend baru) ke canvas XRGB; `dmg` = bbox aktual
untuk `gui_damage_rect()` **existing** (tanpa damage subsystem baru).
Integrasi libui: caller (app/Painter) memanggil `kz_text_draw` lalu
`gui_damage_rect` — `Painter::text`/`gui_draw_text` 8x16 TIDAK diubah
(nol regress; AA opt-in per call-site, berikutnya: label/button/title).

## 7. UTF-8 handling (Phase 7)

Audit: repo BELUM punya decoder (semua jalur `char`, >127 → `?`).
`kz_utf8_decode` milik libtext: 1..4 byte, overlong/surrogate/truncated
→ U+FFFD (1 byte), tak pernah over-read. HarfBuzz/shaping: TIDAK
(fase awal, sesuai instruksi).

## 8. Thread safety (Phase 18)

Tanpa state global di manager (satu `kz_font_t` per task, tanpa lock).
Backend FT: satu `FT_Library` + face satu slot per proses; app Kyuzen
single-threaded per task → aman; batas didokumentasikan di
`kzraster_ft.c` (upgrade = lock di sekitar FT calls bila perlu).

## 9. Fallback behavior (Phase 19/20)

Invalid font / corrupt / glyph missing / OOM / UTF-8 rusak → return
`<0`/`NULL`, glyph di-skip, TIDAK panic (dikunci host test §7).
Tanpa backend: measure valid (width 0), draw no-op. Caller fallback =
bitmap `font8x16` existing atau box `U+FFFD` (kebijakan di caller).

## 10. Fonts (5 TTF redistributable — host test + demo embed)

`assets/fonts/`: DejaVuSans.ttf (757.076 byte, DejaVu Sans 2.37,
Bitstream Vera + public-domain, sumber release resmi
`dejavu-fonts-ttf-2.37.zip`), Inter-Regular.ttf (411.640 byte, OFL),
NotoSansAdlam-Regular.ttf (60.276 byte, OFL, cakupan Adlam saja —
tanpa 'A'/latin: bahan uji missing-glyph), NotoSansMono-Regular +
-Bold.ttf (~406KB, OFL). Semua format TTF, non-variable
(verifikasi via audit FT host). Lisensi menyertai
(DejaVuSans-LICENSE.txt, LICENSE.txt/OFL.txt). Kelimanya di-embed
ke fontdemo via `ld -b binary` (bukan bundled generik OS — khusus
demo; font proprietary tidak ada di tree ini).

## 11. GPU compatibility (Phase 17)

Glyph = coverage + blend via jalur canvas existing → semua backend
GHAL (software/virtio/intel) tak tersentuh; software fallback selalu
ada (jalur ini SATU-SATUNYA jalur — tanpa GPU-specific code).

## 12. Verifikasi

* `make test-text` — host test mock 7 grup, ALL PASS (tak regress
  oleh perubahan `kzraster_ft.c`).
* `make test-text-ft` — BRING-UP: archive freestanding terbangun +
  host test real-FT ALL PASS. DejaVu Sans 'A' 16px: measure w=11
  h=19 (asc−desc), draw 68px tersentuh, bbox 11×12, inti opaque
  16px + 52px tepi AA (grayscale 8-bit terbukti, bukan 1-bit);
  cache 1 miss lalu hits; blob rusak graceful (width 0, no crash).
* `make all` (kernel) — exit 0, tak regress (kernel tak tersentuh).
* QEMU raster nyata — NOT VERIFIED (tanpa harness ELF demo; sesuai
  scope: Phase 12/13 memutuskan STOP dan lapor — tidak ada klaim
  QEMU PASS dari compile saja).

## 13. Bring-up record (Phase 19)

```text
FreeType version: 2.14.3 (FREETYPE_MAJOR/MINOR/PATCH, third_party/freetype/)
Build: freestanding / x86_64-pc-none-elf (-nostdlib, -msoft-float, -O2)
Allocator: Kyuzen dynamic allocator (kz_heap_t -> FT_MemoryRec_; tanpa arena khusus)
Font input: memory-backed blob (FT_New_Memory_Face; tanpa fopen/VFS)
Rasterization: TrueType + grayscale 8-bit (smooth) + embedded-zlib gzip
Shaping: not included (layout = advance horizontal + baseline)
Filesystem: not required (file stream ditolak jujur: Cannot_Open_Resource)
Thread model: single-flight per process (satu FT_Library + face satu slot)
```

Known limitations: `FT_Init_FreeType`/`FT_Done_FreeType` tak didukung
(pakai `FT_New_Library` + heap eksplisit); trace/debug output mati;
tanpa autofit/CFF/Type1/WOFF2-Brotli; cache full-drop bukan LRU;
`ft_longjmp` port hanya nilai 1 (semua call-site upstream = 1).

## 14. Font Demo ELF (apps/fontdemo.c)

Demo userspace libui: 5 font embedded (`ld -b binary` -> blob ->
`FT_New_Memory_Face`, tanpa array byte manual) + widget `FtText`
baru (`libs/gui/widget/.../fttext.*` + ABI `ui_fttext_*` — toolkit
tetap FT-free, rasterisasi milik app via callback). Satu `kz_font_t`
per ukuran (cache key px); ganti font runtime (tombol `[<]`/`[>]`,
panah Left 0x14B/Right 0x14D) = destroy + load + `ui_fttext_refresh`
(damage existing). Jejak COM1 `[fontdemo]` untuk verifikasi headless.
Registrasi: `apps/Makefile` GUI_APPS + root APP_NAMES + limine.conf
(.elf/.app) + `manifests/fontdemo.app` + icon `font-app.png`
(DESKTOP_ASSETS); launcher menemukan otomatis (tanpa hardcode).

QEMU (tools/fontdemo/run-qemu.sh, `start fontdemo`, sendkey):
launch + 5 switch + Left-back TANPA panic (setelah perbaiki
use-after-free `FT_MemoryRec_.user` menunjuk `kz_heap_t` milik font
yang sudah di-destroy — sekarang salinan statis `g_ft_heap`).
Serial: measure A16 w=11 h=19 (== host); tiap font 26 lookups dengan
HIT 'A' (DejaVu/Inter/Mono/MonoBold 25m+1h; Adlam 26m+0h = missing
glyph graceful). Screenshot 1920x1080: 5 ukuran + UTF-8 + palet
ter-render; antar-font berbeda 14-25k px; 144-149 level abu (AA
bukan 1-bit); Mono Bold paling tebal; 0 px berubah di luar window
(damage contained); Mono==Mono setelah Left-back (deterministik).
Ikon: modul `font-app.png` termuat (`[mod] OK`) + sel-16 desktop
cocok dengan signature PNG; klik-ikon BLOCKED (kursor guest tak
mau absolute-positioning via HMP — terdokumentasi di skrip).

## 15. System font policy (Inter default + Settings + label desktop)

Policy: low-level (kernel/shell/panic/TTY/KWM title/compositor) =
bitmap 8x16 deterministik, TANPA FreeType; high-level (desktop
launcher labels, Settings, fontdemo) = FreeType via libs/text.
Registry tunggal `libs/text/include/kzfonts.h` (id 0 = Inter
Regular default eksplisit; display name; nama file FS) + format
`kzfontcfg.h` ("/font.ui": "KZF1" + id; invalid -> default).
Desktop (`system/desktop/launcher.*`): baca TTF dari FS root (modul
ISO -> FS, pola DESKTOP_ASSETS), satu kz_font_t 16px; label
terpusat via measure (shadow tak ikut layout); shadow dua-pass
(1,1, hitam alpha 90) via `Canvas::draw_raw` baru (libdesktop tetap
buta-font; damage = bbox teks ∪ shadow); reload saat font.ui berubah
(poll ≤5 dtk, Damage::Full, tanpa reboot); fallback bitmap bila font
tak termuat (pixel-identical, dikunci test-desktop). Settings
(`apps/settings.c`): 5 tombol font + preview FtText live + Simpan/
Muat font.ui + shortcut 1-5/S. fontdemo memakai nama/urutan registry
(blob tetap embedded). Batasan arsitektur (disengaja): judul window
KWM/kompositor, taskbar/jam/notifikasi, dan widget libui tetap
bitmap (renderer milik subsistem lain; konversi = milestone sendiri).

QEMU (tools/fontdemo/run-qemu-sysfont.sh): boot default Inter
(`[desktop] uifont Inter Regular`, label 6675px beda vs DejaVu);
Settings preview live Inter->DejaVu (3731px); simpan -> desktop
ikut DejaVu ≤5 dtk (marker + label); kembali Inter -> label
bit-identik (0px diff, deterministik); shadow terbukti (53% piksel
teks punya tetangga +1,+1 lebih gelap di wallpaper terang, offset
dan alpha sesuai kode); 0 panic. Ditemukan & diperbaiki saat
verifikasi: (1) FtText toolkit tak mencatat libgui damage (tulis
canvas langsung) -> layar basi; kini `gui_damage_rect` di draw();
(2) settings memanggil on_font_load sebelum widget ada (NULL label)
-> pindah setelah create + guard; (3) marker serial terbelah task
lain -> trace satu-print + pola tunggu fragmen stabil.
