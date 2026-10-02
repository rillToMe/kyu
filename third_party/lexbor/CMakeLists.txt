# ============================================================================
# third_party/lexbor/CMakeLists.txt — Lexbor 3.0.0, freestanding.
#
# Reproduces third_party/lexbor/kyuzen.mk: the same module closure, the same
# FP-ban, the same include order. Archive lands in
# build/lib/liblexbor_kyuzen.a (the path the old build produced).
#
# WHY SOME MODULES ARE ABSENT (unchanged from the old build):
#   css, selectors, style, engine, encoding, url, unicode, punycode, utils,
#   ports — not needed for the HTML/DOM closure, and css/selectors return
#   `double`, which is "SSE register return with SSE disabled" under the
#   Kyuzen user-space ABI (-mno-sse -msoft-float).
#
# WHY FOUR core FILES ARE EXCLUDED:
#   core/conv.c, core/dtoa.c, core/strtod.c, core/diyfp.c implement IEEE-754
#   double<->text conversion and cannot compile under the same ABI. The single
#   symbol the HTML/DOM closure still needs from that area —
#   lexbor_conv_int64_to_data(), integer only — is provided by
#   kyuzen/src/lexbor_conv_shim.c.
#
# A build-time guard below fails loudly if a banned file ever re-enters the
# source list (the Makefile had the same guard via a shell loop).
# ============================================================================

set(_lexbor_dir "${KYUZEN_ROOT}/third_party/lexbor")
set(_lexbor_src "${_lexbor_dir}/source/lexbor")

# ---------------------------------------------------------------------------
# FP/ABI blockers. These MUST NOT appear in the source list.
# ---------------------------------------------------------------------------
set(_lexbor_fp_banned
    "${_lexbor_src}/core/conv.c"
    "${_lexbor_src}/core/dtoa.c"
    "${_lexbor_src}/core/strtod.c"
    "${_lexbor_src}/core/diyfp.c"
)

# ---------------------------------------------------------------------------
# Module closure. kyuzen_glob resolves each module's pattern at configure time
# — the same per-module wildcard behaviour the Makefile had, without CMake's
# per-build glob verification (see cmake/KyuzenGlob.cmake).
# ---------------------------------------------------------------------------
kyuzen_glob(_lexbor_core "${_lexbor_src}/core/*.c")
kyuzen_glob(_lexbor_dom "${_lexbor_src}/dom/*.c")
kyuzen_glob(_lexbor_dom_iface "${_lexbor_src}/dom/interfaces/*.c")
kyuzen_glob(_lexbor_html "${_lexbor_src}/html/*.c")
kyuzen_glob(_lexbor_html_iface "${_lexbor_src}/html/interfaces/*.c")
kyuzen_glob(_lexbor_html_tok "${_lexbor_src}/html/tokenizer/*.c")
kyuzen_glob(_lexbor_html_tree "${_lexbor_src}/html/tree/*.c")
kyuzen_glob(_lexbor_html_tree_im "${_lexbor_src}/html/tree/insertion_mode/*.c")
kyuzen_glob(_lexbor_ns "${_lexbor_src}/ns/*.c")
kyuzen_glob(_lexbor_tag "${_lexbor_src}/tag/*.c")

# core/ is globbed, so the FP files must be filtered OUT of it — the same
# `$(filter-out $(LEXBOR_FP_BANNED),$(wildcard ...))` the old Makefile used.
list(REMOVE_ITEM _lexbor_core ${_lexbor_fp_banned})

set(_lexbor_sources
    ${_lexbor_core}
    ${_lexbor_dom}
    ${_lexbor_dom_iface}
    ${_lexbor_html}
    ${_lexbor_html_iface}
    ${_lexbor_html_tok}
    ${_lexbor_html_tree}
    ${_lexbor_html_tree_im}
    ${_lexbor_ns}
    ${_lexbor_tag}
    # --- Kyuzen port layer --------------------------------------------------
    ${_lexbor_dir}/kyuzen/src/lexbor_memory.c      # allocator -> libc -> syscalls
    ${_lexbor_dir}/kyuzen/src/lexbor_conv_shim.c   # int64 only
)

# ---------------------------------------------------------------------------
# Guard: fail hard if a banned file slipped into the closure.
# ---------------------------------------------------------------------------
foreach(_banned IN LISTS _lexbor_fp_banned)
    if(_banned IN_LIST _lexbor_sources)
        message(FATAL_ERROR
            "Lexbor: FP/ABI-blocked file entered the build: ${_banned}\n"
            "These return `double` and cannot compile with -mno-sse. "
            "See third_party/lexbor/kyuzen.mk for the rationale.")
    endif()
endforeach()

add_library(kyuzen-lexbor STATIC ${_lexbor_sources})
target_link_libraries(kyuzen-lexbor PRIVATE kyuzen-flags-lexbor)

# Include order is load-bearing: kyuzen/include must come first so the shim
# <memory.h> wins over any host header, then the Lexbor sources, then the
# staged C SDK headers. kyuzen-flags-lexbor already provides the first two in
# the right order; the SDK include is added here because it is staged.
target_include_directories(kyuzen-lexbor PUBLIC
    ${KYUZEN_ROOT}/third_party/lexbor/kyuzen/include
    ${KYUZEN_ROOT}/third_party/lexbor/source
)

set_target_properties(kyuzen-lexbor PROPERTIES
    OUTPUT_NAME "lexbor_kyuzen"
    ARCHIVE_OUTPUT_DIRECTORY "${KYUZEN_LIB_DIR}"
)

# Upstream code we do not modify.
target_compile_options(kyuzen-lexbor PRIVATE -w)
