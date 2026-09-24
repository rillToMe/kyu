# third_party/freetype/kyuzen.mk — FreeType 2.14.3 freestanding untuk KyuzenOS.
#
# STATUS: BRING-UP PASS (2026-09-22). Host test + freestanding archive
# terverifikasi (lihat Evidence di bawah). Di-include root Makefile
# setelah target test-text (vars only — rules di root Makefile).
#
# Desain (Phase 5/16): modul minimal + konfigurasi/adapter Kyuzen,
# TANPA patch source upstream:
#   - src/base: ftbase.c (amalgamasi inti) + ftinit.c (registrasi modul)
#     + ftglyph.c + ftbitmap.c + ftbbox.c + ftgasp.c + ftmm.c
#     (ftmm = FT_Set_Named_Instance, dipakai tt_face_init)
#   - modul font: truetype.c (glyph loading + interpreter bawaan) +
#     sfnt.c (wrapper SFNT; butuh psnames)
#   - raster AA: smooth.c (grayscale 8-bit)
#   - nama glyph: psnames.c
#   - dekompresi: gzip/ftgzip.c (embedded zlib, alokasi via FT_Memory —
#     WOFF/.gz jalan tanpa malloc host)
#   - port Kyuzen: kzf_port.c (libc primitives) + kzf_system.c
#     (pengganti ftsystem.c: no-fopen, no-trace, no-global-allocator)
# SENGAJA MATI: type1/cff/cid/pfr/type42/winfonts/pcf/bdf, autofit,
# pshinter, psaux, cache, gxvalid, otvalid, raster-mono, svg, sdf,
# lzw, bzip2, ftsystem.c hulu, ftdebug.c hulu.
#
# Evidence:
# - host: 14 TU kompil TANPA warning + link + text_ft_real ALL PASS
#   (DejaVuSans 'A' 16px: w=11 h=19, inti 16px + 52px tepi AA).
# - freestanding (x86_64-pc-none-elf, -nostdlib, -msoft-float):
#   16 TU kompil OK; closure simbol: 696 defined, 110 undefined yang
#   semuanya internal, 0 EXTERNAL (llvm-nm) — nol dependensi host libc
#   (tanpa memcpy/malloc/float-helper global).
#
# Requires dari root Makefile: BUILD_DIR, OBJ_DIR (terdefinisi sebelum
# titik include). Output TIDAK masuk ALL_OBJS kernel (libtext =
# userspace; kernel tetap FT-free sesuai docs/design/font-rendering.md).

FT_KYUZEN_SRCS = \
    third_party/freetype/src/base/ftbase.c \
    third_party/freetype/src/base/ftinit.c \
    third_party/freetype/src/base/ftglyph.c \
    third_party/freetype/src/base/ftbitmap.c \
    third_party/freetype/src/base/ftbbox.c \
    third_party/freetype/src/base/ftgasp.c \
    third_party/freetype/src/base/ftmm.c \
    third_party/freetype/src/truetype/truetype.c \
    third_party/freetype/src/sfnt/sfnt.c \
    third_party/freetype/src/smooth/smooth.c \
    third_party/freetype/src/psnames/psnames.c \
    third_party/freetype/src/gzip/ftgzip.c \
    third_party/freetype/kyuzen/src/kzf_port.c \
    third_party/freetype/kyuzen/src/kzf_system.c

FT_KYUZEN_HDRS = \
    third_party/freetype/kyuzen/include/ftkz_stdlib.h \
    third_party/freetype/kyuzen/include/ftkz_modules.h \
    third_party/freetype/kyuzen/include/string.h \
    third_party/freetype/kyuzen/include/setjmp.h \
    libs/text/include/kzfont.h

# Mekanisme resmi ftheader.h/ftinit.c (tanpa patch upstream):
# - FT_CONFIG_STANDARD_LIBRARY_H -> port libc Kyuzen (kzf_*)
# - FT_CONFIG_MODULES_H -> 4 modul (tt/sfnt/psnames/smooth)
# - KZFONT_USE_FREETYPE -> backend penuh di kzraster_ft.c
# Bentuk <...> single-quoted: sh me-strip kutip luar sehingga clang
# menerima -D...=<header> utuh (kutip ganda di dalam Makefile akan
# dimakan sh dan makro menjadi bare token -> #include gagal).
FT_KYUZEN_DEFS = -DFT2_BUILD_LIBRARY \
    -D'FT_CONFIG_STANDARD_LIBRARY_H=<ftkz_stdlib.h>' \
    -D'FT_CONFIG_MODULES_H=<ftkz_modules.h>' \
    -DKZFONT_USE_FREETYPE

FT_KYUZEN_INCS = -Ithird_party/freetype/kyuzen/include \
    -Ithird_party/freetype/include \
    -Ilibs/text/include \
    -Ilibs/gui/color/include

# Flag == kebijakan userspace existing (apps/Makefile CFLAGS_LIB):
# freestanding + -mno-sse* + -msoft-float + -O2. BUKAN CFLAGS kernel
# (-mcmodel=kernel salah untuk userspace). -MMD -MP untuk .d tracking.
FT_KYUZEN_CFLAGS = --target=x86_64-pc-none-elf -ffreestanding -nostdlib \
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -O2 -std=c11 \
    -MMD -MP $(FT_KYUZEN_DEFS) $(FT_KYUZEN_INCS)

FT_KYUZEN_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(FT_KYUZEN_SRCS))
FT_KYUZEN_A = $(BUILD_DIR)/lib/libfreetype_kyuzen.a
