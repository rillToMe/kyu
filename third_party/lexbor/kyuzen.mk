# third_party/lexbor/kyuzen.mk - Lexbor 3.0.0 freestanding untuk KyuzenOS.
#
# STATUS: STAGE A (vendor + build + host/QEMU smoke probe). HTML/DOM
# parser only - TIDAK ada perubahan perilaku browser, TIDAK ada Duktape,
# TIDAK ada perubahan kernel.
#
# DESAIN (mirror third_party/freetype/kyuzen.mk, TANPA patch upstream):
#   - Modul yang di-vendor & dibangun: core, dom, html, ns, tag.
#     Ini adalah closure lengkap yang dibutuhkan untuk
#     lxb_html_parse() -> DOM tree + DOM lookup + serialisasi.
#   - SENGAJA TIDAK di-vendor/dibangun: css, selectors, style, engine,
#     encoding, url, unicode, punycode, utils, ports.
#     Alasan: (a) tidak dipakai closure HTML/DOM; (b) css/selectors
#     mengembalikan `double` -> "SSE register return with SSE disabled"
#     di bawah ABI userspace Kyuzen (-mno-sse -msoft-float).
#   - core/{conv,dtoa,strtod,diyfp}.c DIKELUARKAN: seluruh file itu
#     mengimplementasikan IEEE-754 double<->text dan gagal compile di
#     ABI Kyuzen. Satu-satunya simbol dari sana yang dipakai closure
#     HTML/DOM adalah lexbor_conv_int64_to_data() (integer murni),
#     yang disediakan oleh port kyuzen/src/lexbor_conv_shim.c.
#   - Port Kyuzen (kyuzen/src): lexbor_memory.c (allocator hooks ->
#     libc malloc/calloc/realloc/free -> sys_alloc/sys_free),
#     lexbor_conv_shim.c (int64 saja).
#   - Port include (kyuzen/include): memory.h shim (base.h memakai
#     <memory.h> yang bukan bagian SDK freestanding; tanpa shim ini
#     preprocessor diam-diam jatuh ke header libc HOST /usr/include).
#
# CATATAN ABI: -DLEXBOR_STATIC menghindari dekorasi dllexport/import
# pada Windows-host preprocessing; harmless di ELF. Tidak ada patch
# source: hanya include-path + definisi.
#
# Requires dari root Makefile: BUILD_DIR, OBJ_DIR, LIBC_CC,
# LIBC_TARGET_FLAGS, SDK_INC (terdefinisi sebelum titik include).
# Output TIDAK masuk ALL_OBJS kernel (Lexbor = userspace).

LEXBOR_DIR = third_party/lexbor

# File yang HARUS absen dari build (FP/ABI blocker: mengembalikan
# `double` -> "SSE register return with SSE disabled").
LEXBOR_FP_BANNED = \
    $(LEXBOR_DIR)/source/lexbor/core/conv.c \
    $(LEXBOR_DIR)/source/lexbor/core/dtoa.c \
    $(LEXBOR_DIR)/source/lexbor/core/strtod.c \
    $(LEXBOR_DIR)/source/lexbor/core/diyfp.c

# Wildcard per-modul (deterministik; upstream tidak menambah file tanpa
# mengubah struktur). Urutan modul tidak penting untuk ar.
# core/*.c di-filter-out dari LEXBOR_FP_BANNED (conv/dtoa/strtod/diyfp);
# simbol tunggal yang masih dipakai closure HTML/DOM disediakan oleh
# kyuzen/src/lexbor_conv_shim.c.
LEXBOR_KYUZEN_SRCS = \
    $(filter-out $(LEXBOR_FP_BANNED),$(wildcard $(LEXBOR_DIR)/source/lexbor/core/*.c)) \
    $(wildcard $(LEXBOR_DIR)/source/lexbor/dom/*.c) \
    $(wildcard $(LEXBOR_DIR)/source/lexbor/dom/interfaces/*.c) \
    $(wildcard $(LEXBOR_DIR)/source/lexbor/html/*.c) \
    $(wildcard $(LEXBOR_DIR)/source/lexbor/html/interfaces/*.c) \
    $(wildcard $(LEXBOR_DIR)/source/lexbor/html/tokenizer/*.c) \
    $(wildcard $(LEXBOR_DIR)/source/lexbor/html/tree/*.c) \
    $(wildcard $(LEXBOR_DIR)/source/lexbor/html/tree/insertion_mode/*.c) \
    $(wildcard $(LEXBOR_DIR)/source/lexbor/ns/*.c) \
    $(wildcard $(LEXBOR_DIR)/source/lexbor/tag/*.c) \
    $(LEXBOR_DIR)/kyuzen/src/lexbor_memory.c \
    $(LEXBOR_DIR)/kyuzen/src/lexbor_conv_shim.c

LEXBOR_KYUZEN_HDRS = \
    $(LEXBOR_DIR)/kyuzen/include/memory.h

# Include order penting:
#   1. kyuzen/include  -> shim <memory.h> HARUS menang atas host libc
#   2. source          -> "lexbor/core/base.h" dst.
#   3. SDK_INC         -> freestanding Kyuzen libc (<stdlib.h>, <string.h>)
LEXBOR_KYUZEN_INCS = \
    -I$(LEXBOR_DIR)/kyuzen/include \
    -I$(LEXBOR_DIR)/source \
    -isystem $(SDK_INC)

# Flag == kebijakan userspace existing (apps/Makefile / BL_OS_FLAGS):
# freestanding + -mno-sse* + -msoft-float + -O2. -MMD -MP untuk .d.
# -nostdinc TIDAK dipakai (SDK_INC via -isystem + kyuzen shim sudah
# menutup <memory.h>); konsisten dengan build BearSSL yang ada.
LEXBOR_KYUZEN_CFLAGS = $(LIBC_TARGET_FLAGS) -O2 -std=c11 \
    -DLEXBOR_STATIC -MMD -MP $(LEXBOR_KYUZEN_INCS)

LEXBOR_KYUZEN_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXBOR_KYUZEN_SRCS))
LEXBOR_KYUZEN_A = $(BUILD_DIR)/lib/liblexbor_kyuzen.a

# Guard: fail keras bila file FP bocor ke daftar sumber.
define lexbor_check_no_fp
	@for f in $(LEXBOR_FP_BANNED); do \
	    case " $(LEXBOR_KYUZEN_SRCS) " in \
	        *" $$f "*) echo "[lexbor] FAIL: FP file masuk build: $$f"; exit 1;; \
	    esac; \
	done
endef
