# ============================================================================
# third_party/CMakeLists.txt
#
# Third-party integration follows docs/development/build-system-audit.md §8. Each dependency is
# integrated the way the existing build already integrates it — no upstream
# build system is forced onto the bare-metal target, and no new dependency is
# introduced.
#
# Classification (audit §8):
#   lwIP       D + G  vendored sources compiled directly, freestanding config
#   FreeType   A + G  has CMake upstream, but the project uses a curated module
#                     list + port layer instead
#   Lexbor     D + G  curated module closure, FP sources banned
#   BearSSL    D + E + F  host tool AND a separate minimal OS subset
#   LLVM libc  A + G  nested CMake build driven as an external step
# ============================================================================

# ---------------------------------------------------------------------------
# lwIP — kernel network stack.
#
# Compiled with kyuzen-flags-lwip (kernel flags + lwIP include paths +
# -Wno-error). The source set is the EXACT set the old Makefile's wildcards
# resolved to: core/*.c + core/ipv4/*.c + netif/ethernet.c + the two Kyuzen
# port files. Deliberately excluded (unchanged from the old build):
#   slipif.c, zepif.c, bridgeif*.c, lowpan6*.c, ppp/
# ---------------------------------------------------------------------------
set(_lwip_dir "${KYUZEN_ROOT}/third_party/net/lwip/src")

kyuzen_glob(_lwip_core "${_lwip_dir}/core/*.c")
kyuzen_glob(_lwip_ipv4 "${_lwip_dir}/core/ipv4/*.c")

# file(GLOB) order is unspecified; Make's $(wildcard) is sorted. Sort so the
# object order (and therefore the linked binary) is deterministic and matches
# the baseline.
list(SORT _lwip_core)
list(SORT _lwip_ipv4)

add_library(kyuzen-lwip OBJECT
    ${_lwip_core}
    ${_lwip_ipv4}
    ${_lwip_dir}/netif/ethernet.c
    ${KYUZEN_ROOT}/drivers/net/port/kyuzen_netif.c
    ${KYUZEN_ROOT}/drivers/net/port/sys_arch.c
)
target_link_libraries(kyuzen-lwip PRIVATE kyuzen-flags-lwip)
target_include_directories(kyuzen-lwip PRIVATE ${KYUZEN_ROOT}/include)

# ---------------------------------------------------------------------------
# e1000 — Intel 82540EM NIC driver.
#
# Standalone: it needs no lwIP headers, so it compiles with the plain kernel
# flags. (The old Makefile had a comment making this distinction explicit.)
# ---------------------------------------------------------------------------
add_library(kyuzen-e1000 OBJECT "${KYUZEN_ROOT}/drivers/net/e1000/e1000.c")
target_link_libraries(kyuzen-e1000 PRIVATE kyuzen-flags-kernel)
target_include_directories(kyuzen-e1000 PRIVATE ${KYUZEN_ROOT}/include)

# ---------------------------------------------------------------------------
# FreeType — freestanding static library.
#
# Integrated via include(kyuzen.cmake), NOT add_subdirectory: the directory
# already contains an UPSTREAM CMakeLists.txt (644 lines) that must not be
# modified or used (it targets a hosted environment). The Kyuzen integration
# lives in a separate file, mirroring the existing `kyuzen.mk` naming.
#
# Uses the project's curated module list (14 upstream TUs + 2 Kyuzen port TUs).
# Deliberately excluded upstream modules (unchanged): type1, cff, cid, pfr,
# type42, winfonts, pcf, bdf, autofit, pshinter, psaux, cache, gxvalid,
# otvalid, raster-mono, svg, sdf, lzw, bzip2, and the upstream ftsystem.c /
# ftdebug.c.
# ---------------------------------------------------------------------------
include(${KYUZEN_ROOT}/third_party/freetype/kyuzen.cmake)

# ---------------------------------------------------------------------------
# Lexbor — freestanding static library (HTML/DOM parser).
#
# Module closure from third_party/lexbor/kyuzen.mk: core, dom, html, ns, tag.
# css/selectors/style/engine/encoding/url/unicode/punycode/utils/ports are NOT
# vendored or built (they return `double`, which cannot compile under the
# -mno-sse ABI).
#
# The four FP core sources are banned; a single int64 symbol they would have
# provided is supplied by kyuzen/src/lexbor_conv_shim.c.
# ---------------------------------------------------------------------------
add_subdirectory(lexbor)

# ---------------------------------------------------------------------------
# BearSSL — two independent builds (audit §8).
#   host:  full library -> brssl tool -> generates trust-anchor C arrays
#   OS:    minimal TLS client subset linked into browser.elf
# ---------------------------------------------------------------------------
add_subdirectory(bearssl)
