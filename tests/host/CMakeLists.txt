# ============================================================================
# tests/host/CMakeLists.txt — host-side unit tests.
#
# THIS IS A SEPARATE CMake INVOCATION WITH THE HOST TOOLCHAIN.
#
#   cmake -S tests/host -B build/host -G Ninja
#   ctest --test-dir build/host
#
# It must NOT inherit cmake/KyuzenToolchain.cmake: that file sets the
# bare-metal triple and CMAKE_TRY_COMPILE_TARGET_TYPE, which would make the
# tests target x86_64-pc-none-elf and fail to link against the host libc
# (Rule 16 — host tests stay host builds).
#
# Most tests compile the REAL production sources against mocks rather than
# copies of the logic, so a bug in the kernel/library code shows up here. The
# `-iquote` vs `-I` distinction is deliberate: the repo's include/ contains a
# stdlib.h shim (stb_image) that would shadow the host's <stdlib.h> and break
# libc++ headers.
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Force clang/clang++.
#
# The old Makefile used HOSTCC=clang, HOSTCXX=clang++. CMake's default
# detection would pick `cc`/`c++`, which on this host are GCC - and the test
# suite relies on clang behaviour (e.g. colour_test.c's _Static_assert on a
# struct member, which GCC rejects as a non-constant expression).
#
# Setting the compilers before project() keeps detection deterministic.
# ---------------------------------------------------------------------------
find_program(KYUZEN_HOST_CLANG   NAMES clang   REQUIRED)
find_program(KYUZEN_HOST_CLANGXX NAMES clang++ REQUIRED)

# ---------------------------------------------------------------------------
# Depfile normalisation — the same wrapper the target build uses.
#
# These are HOST compiles, but the compiler is still MSYS2's usr/bin clang, so
# its depfiles still carry POSIX paths (/usr/include/string.h, /usr/lib/clang/
# 21/include/stddef.h). Native Ninja cannot stat those, treats them as missing,
# and rebuilds all 645 steps on every invocation — a no-op `ninja -C build-host`
# never settles.
#
# The wrapper is shared rather than copied: it is triple-agnostic (it runs the
# real compiler with whatever arguments it is handed and only rewrites the
# resulting depfile), so it works identically for host and bare-metal compiles.
# It is pulled from cmake/KyuzenCompilerWrapper.cmake, NOT from
# KyuzenToolchain.cmake, which would switch these tests to the bare-metal ABI.
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/KyuzenCompilerWrapper.cmake")
kyuzen_install_compiler_wrapper("${KYUZEN_HOST_CLANG}" "${KYUZEN_HOST_CLANGXX}"
                                 "${CMAKE_BINARY_DIR}/tools")

# UI XML embedding (file .xml -> header) — dipakai test-libui-xml supaya test
# dan aplikasi menguji DOKUMEN YANG SAMA (tanpa salinan string yang bisa drift).
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/KyuzenUiXml.cmake")

set(CMAKE_C_COMPILER   "${KYUZEN_WRAPPED_CC}")
set(CMAKE_CXX_COMPILER "${KYUZEN_WRAPPED_CXX}")

project(KyuzenHostTests LANGUAGES C CXX)

# Compilation database for IntelliSense. The target build sets this in the
# top-level CMakeLists.txt; this is a separate invocation, so it needs its own.
# The two databases describe different flag sets (host vs bare-metal) and are
# not interchangeable — an IDE pointed at the wrong one will show phantom
# errors on every include.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Paths back to the repository root. tests/host/CMakeLists.txt is two levels
# down, so the root is ../.. relative to this file.
# ---------------------------------------------------------------------------
get_filename_component(KYUZEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(KYUZEN_UNIT_DIR "${KYUZEN_ROOT}/tests/host/unit")

# ---------------------------------------------------------------------------
# kyuzen_glob() — shared with the target build.
#
# This is a SEPARATE CMake invocation, so it does not inherit anything the
# top-level CMakeLists.txt includes; the helper must be pulled in explicitly
# or every kyuzen_glob() call below fails with "Unknown CMake command".
#
# Only the glob helper is shared. KyuzenToolchain.cmake deliberately is NOT
# (it would switch these tests to the bare-metal triple — see the header).
# ---------------------------------------------------------------------------
include("${KYUZEN_ROOT}/cmake/KyuzenGlob.cmake")

enable_testing()

# ---------------------------------------------------------------------------
# Shared helper: register a host test.
#
#   kyuzen_host_test(<name> SOURCES <src>... [DEFINES ...] [INCLUDES ...]
#                    [IQUOTE ...] [COMPILE_OPTIONS ...] [LINK_LIBS ...])
#
# INCLUDES become -I, IQUOTE become -iquote. The distinction matters (see the
# header comment). Every test is registered with CTest under its Makefile
# target name so the two build systems report the same test names.
# ---------------------------------------------------------------------------
function(kyuzen_host_test name)
    cmake_parse_arguments(ARG
        ""
        ""
        "SOURCES;DEFINES;INCLUDES;IQUOTE;COMPILE_OPTIONS;LINK_LIBS"
        ${ARGN}
    )

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "kyuzen_host_test(${name}): SOURCES is required")
    endif()

    add_executable(${name} ${ARG_SOURCES})

    if(ARG_DEFINES)
        target_compile_definitions(${name} PRIVATE ${ARG_DEFINES})
    endif()
    if(ARG_INCLUDES)
        target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
    endif()
    if(ARG_IQUOTE)
        # -iquote takes exactly ONE directory per flag. Passing a list after a
        # single -iquote makes the extra directories look like input files, so
        # one -iquote is emitted per directory using the SHELL: grouping prefix.
        foreach(_d IN LISTS ARG_IQUOTE)
            target_compile_options(${name} PRIVATE "SHELL:-iquote ${_d}")
        endforeach()
    endif()
    if(ARG_COMPILE_OPTIONS)
        target_compile_options(${name} PRIVATE ${ARG_COMPILE_OPTIONS})
    endif()
    if(ARG_LINK_LIBS)
        target_link_libraries(${name} PRIVATE ${ARG_LINK_LIBS})
    endif()

    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_test(NAME ${name} COMMAND ${name})
endfunction()

# ===========================================================================
# BearSSL host library — needed by the TLS test and the browser TLS path.
# Full library, host ABI, exactly as the old Makefile built it for the host.
# ===========================================================================
kyuzen_glob(_bl_host_srcs
    "${KYUZEN_ROOT}/third_party/bearssl/src/*/*.c")
list(APPEND _bl_host_srcs "${KYUZEN_ROOT}/third_party/bearssl/src/settings.c")

add_library(kyuzen-host-bearssl STATIC ${_bl_host_srcs})
target_include_directories(kyuzen-host-bearssl PUBLIC
    "${KYUZEN_ROOT}/third_party/bearssl/inc"
    "${KYUZEN_ROOT}/third_party/bearssl/src"
)
target_compile_options(kyuzen-host-bearssl PRIVATE -O1 -w)

# ===========================================================================
# Lexbor host library — the browser engine host tests link the SAME Lexbor
# sources the freestanding build uses, so the logic under test is identical.
# The FP-blocked files stay excluded and the int64 shim is included, matching
# third_party/lexbor/kyuzen.mk.
# ===========================================================================
set(_lexbor_dir "${KYUZEN_ROOT}/third_party/lexbor")
set(_lexbor_src "${_lexbor_dir}/source/lexbor")

set(_lexbor_fp_banned
    "${_lexbor_src}/core/conv.c"
    "${_lexbor_src}/core/dtoa.c"
    "${_lexbor_src}/core/strtod.c"
    "${_lexbor_src}/core/diyfp.c"
)

kyuzen_glob(_lexbor_core "${_lexbor_src}/core/*.c")
list(REMOVE_ITEM _lexbor_core ${_lexbor_fp_banned})

kyuzen_glob(_lexbor_rest
    "${_lexbor_src}/dom/*.c"
    "${_lexbor_src}/dom/interfaces/*.c"
    "${_lexbor_src}/html/*.c"
    "${_lexbor_src}/html/interfaces/*.c"
    "${_lexbor_src}/html/tokenizer/*.c"
    "${_lexbor_src}/html/tree/*.c"
    "${_lexbor_src}/html/tree/insertion_mode/*.c"
    "${_lexbor_src}/ns/*.c"
    "${_lexbor_src}/tag/*.c"
)
list(SORT _lexbor_core)
list(SORT _lexbor_rest)

add_library(kyuzen-host-lexbor STATIC
    ${_lexbor_core}
    ${_lexbor_rest}
    "${_lexbor_dir}/kyuzen/src/lexbor_memory.c"
    "${_lexbor_dir}/kyuzen/src/lexbor_conv_shim.c"
)
target_include_directories(kyuzen-host-lexbor PUBLIC
    "${_lexbor_dir}/kyuzen/include"
    "${_lexbor_dir}/source"
)
target_compile_definitions(kyuzen-host-lexbor PUBLIC LEXBOR_STATIC)
target_compile_options(kyuzen-host-lexbor PRIVATE -O1 -w)

# ===========================================================================
# Tests
# ===========================================================================

# --- credential / process / fd / pipe / fork policy (header-only or mock) ---
# These targets are among the ones whose sources are MISSING from the
# repository (see docs/development/build-system-audit.md §13.1). They are declared with an
# EXISTS guard so the breakage is reported at configure time instead of
# producing "No rule to make target" at build time.
#
# Two parallel lists rather than "name;source" pairs: CMake splits a `set()`
# value on semicolons, so a semicolon-joined pair does not survive.
set(_missing_test_names
    test-cred
    test-proc
    test-kill
    test-fd
    test-pipe
    test-fork
    test-virtqueue
    test-virtio-cmd
    test-kyuzenfs-v4
    test-kyuzenfs-xcheck
    test-panic
    test-ata
    test-textedit
)
set(_missing_test_srcs
    cred_test.c
    proc_test.c
    kill_test.c
    fd_test.c
    pipe_test.c
    fork_test.c
    virtqueue_test.c
    virtio_gpu_cmd_test.c
    kyuzenfs_v4_test.c
    kyuzenfs_xcheck.c
    panic_test.c
    ata_devmodel_test.c
    textedit_test.cpp
)

list(LENGTH _missing_test_names _missing_count)
math(EXPR _missing_last "${_missing_count} - 1")

foreach(_i RANGE 0 ${_missing_last})
    list(GET _missing_test_names ${_i} _tname)
    list(GET _missing_test_srcs  ${_i} _tsrc)

    if(NOT EXISTS "${KYUZEN_UNIT_DIR}/${_tsrc}")
        message(WARNING
            "${_tname}: SKIPPED — source ${_tsrc} is not in the repository.\n"
            "  This target is dead in the Make build too (docs/development/build-system-audit.md "
            "§13.1). Restore the source to re-enable it.")
    endif()
endforeach()

# --- net socket lifecycle: REAL kernel/net/net_socket.c against lwIP stubs ---
# The test #includes the production source directly (that is how it reaches the
# kernel code with mocks in place), so the source is NOT listed as a separate
# compilation unit — doing so would duplicate every symbol at link time.
if(EXISTS "${KYUZEN_UNIT_DIR}/netsock_test.c")
    kyuzen_host_test(test-netsock
        SOURCES "${KYUZEN_UNIT_DIR}/netsock_test.c"
        DEFINES KSOCK_HOST_TEST
        IQUOTE "${KYUZEN_UNIT_DIR}/netstub" "${KYUZEN_ROOT}/include" "${KYUZEN_UNIT_DIR}"
        COMPILE_OPTIONS -O1 -Wall -Wextra
    )
endif()

# --- DNS resolver: REAL kernel/net/net_dns.c against lwIP dns.h stubs ---
if(EXISTS "${KYUZEN_UNIT_DIR}/netdns_test.c")
    kyuzen_host_test(test-netdns
        SOURCES "${KYUZEN_UNIT_DIR}/netdns_test.c"
        DEFINES NETDNS_HOST_TEST
        IQUOTE "${KYUZEN_UNIT_DIR}/netstub" "${KYUZEN_ROOT}/include" "${KYUZEN_UNIT_DIR}"
        COMPILE_OPTIONS -O1 -Wall -Wextra
    )
endif()

# --- netutil: REAL libs/core/netutil.c with stubbed syscalls ---
if(EXISTS "${KYUZEN_UNIT_DIR}/netutil_test.c")
    kyuzen_host_test(test-netutil
        SOURCES "${KYUZEN_UNIT_DIR}/netutil_test.c"
        IQUOTE "${KYUZEN_ROOT}/include" "${KYUZEN_UNIT_DIR}"
        COMPILE_OPTIONS -O1 -Wall -Wextra
    )
endif()

# --- browser engine: URL + HTTP (pure C++17, sources are amalgamated) ---
if(EXISTS "${KYUZEN_UNIT_DIR}/browser_url_http_test.cpp")
    kyuzen_host_test(test-browser-url-http
        SOURCES "${KYUZEN_UNIT_DIR}/browser_url_http_test.cpp"
        COMPILE_OPTIONS -std=c++17 -O1 -Wall -Wextra
    )
endif()

# --- browser engine: HTML + DOM via the Lexbor adapter ---
if(EXISTS "${KYUZEN_UNIT_DIR}/browser_html_test.cpp")
    kyuzen_host_test(test-browser-html
        SOURCES
            "${KYUZEN_UNIT_DIR}/browser_html_test.cpp"
            "${KYUZEN_ROOT}/apps/browser/engine/lexbor_html_adapter.cpp"
        INCLUDES
            "${_lexbor_dir}/kyuzen/include"
            "${_lexbor_dir}/source"
        DEFINES LEXBOR_STATIC
        COMPILE_OPTIONS -std=c++17 -O1 -Wall -Wextra
        LINK_LIBS kyuzen-host-lexbor
    )
endif()

# --- browser engine: CSS cascade + layout + hit test ---
if(EXISTS "${KYUZEN_UNIT_DIR}/browser_css_layout_test.cpp")
    kyuzen_host_test(test-browser-css-layout
        SOURCES
            "${KYUZEN_UNIT_DIR}/browser_css_layout_test.cpp"
            "${KYUZEN_ROOT}/apps/browser/engine/lexbor_html_adapter.cpp"
        INCLUDES
            "${_lexbor_dir}/kyuzen/include"
            "${_lexbor_dir}/source"
        DEFINES LEXBOR_STATIC
        COMPILE_OPTIONS -std=c++17 -O1 -Wall -Wextra
        LINK_LIBS kyuzen-host-lexbor
    )
endif()

# --- TLS: BearSSL host library + the generated trust anchors ---
#
# The Makefile's test-tls target does NOT run the binary directly: it runs
#     python tests/browser_site/run_tls_test.py
# which starts a TLS server, points the test at it, and checks the handshake.
# Running the bare binary fails on the handshake steps (err=-999) because
# there is no server listening — so the harness is what CTest must invoke.
if(EXISTS "${KYUZEN_UNIT_DIR}/tls_test.c")
    kyuzen_host_test(test-tls-bin
        SOURCES "${KYUZEN_UNIT_DIR}/tls_test.c"
        INCLUDES
            "${KYUZEN_ROOT}/third_party/bearssl/inc"
            "${KYUZEN_ROOT}/apps/browser/tls"
        COMPILE_OPTIONS -O1 -Wall -Wextra
        LINK_LIBS kyuzen-host-bearssl
    )
    # Rename the CTest entry to the Makefile's target name and drive it through
    # the Python harness. The binary is a build dependency, not the test command.
    set_tests_properties(test-tls-bin PROPERTIES DISABLED TRUE)

    find_program(KYUZEN_PYTHON NAMES python python3)
    if(KYUZEN_PYTHON AND EXISTS "${KYUZEN_ROOT}/tests/browser_site/run_tls_test.py")
        add_test(NAME test-tls
            COMMAND "${KYUZEN_PYTHON}" "${KYUZEN_ROOT}/tests/browser_site/run_tls_test.py"
            WORKING_DIRECTORY "${KYUZEN_ROOT}"
        )
        set_tests_properties(test-tls PROPERTIES
            DEPENDS test-tls-bin
            ENVIRONMENT "KYUZEN_TLS_TEST=$<TARGET_FILE:test-tls-bin>"
        )
    endif()
endif()

# --- colour math: REAL libs/gui/color sources + a C++ header check ---
if(EXISTS "${KYUZEN_UNIT_DIR}/color_test.c")
    kyuzen_host_test(test-color
        SOURCES
            "${KYUZEN_UNIT_DIR}/color_test.c"
            "${KYUZEN_ROOT}/libs/gui/color/src/color_blend.c"
            "${KYUZEN_ROOT}/libs/gui/color/src/color_space.c"
            "${KYUZEN_ROOT}/libs/gui/color/src/color_utils.c"
        INCLUDES
            "${KYUZEN_ROOT}/include"
            "${KYUZEN_ROOT}/libs/gui/color/include"
        COMPILE_OPTIONS -O2 -Wall -Wextra
    )
    # The headers must also be valid C++ (user-space apps are C++).
    # This mirrors the second command in the Makefile's colour test rule.
    #
    # The include flag uses the JOINED form (-I<dir>, no space and no quotes):
    # a quoted "path" becomes an argument with embedded quote characters once
    # CTest writes the command line, and the compiler then cannot find the
    # directory.
    #
    # CMAKE_CXX_COMPILER is a LIST when the depfile-normalising wrapper is in
    # use (sh;build-tools/kyuzen-cxx;clang++). add_test(COMMAND ...) would turn
    # each element into a separate argv entry, so sh would be handed
    # "-std=c++17" as its own option and fail with "-d: invalid option".
    # Only the real compiler is needed here: this is a syntax-only check, it
    # produces no object file and therefore no depfile to normalise.
    if(EXISTS "${KYUZEN_UNIT_DIR}/color_cxx_check.cpp")
        add_test(NAME test-color-cxx-headers
            COMMAND "${KYUZEN_HOST_CLANGXX}"
                    -std=c++17 -Wall -Wextra -fsyntax-only
                    -I${KYUZEN_ROOT}/libs/gui/color/include
                    "${KYUZEN_UNIT_DIR}/color_cxx_check.cpp"
        )
    endif()
endif()

# --- text: REAL libs/text sources with the mock raster backend ---
if(EXISTS "${KYUZEN_UNIT_DIR}/text_test.c")
    kyuzen_host_test(test-text
        SOURCES
            "${KYUZEN_UNIT_DIR}/text_test.c"
            "${KYUZEN_ROOT}/libs/text/src/kzfont.c"
            "${KYUZEN_ROOT}/libs/text/src/kzraster_ft.c"
        INCLUDES
            "${KYUZEN_ROOT}/libs/text/include"
            "${KYUZEN_ROOT}/libs/gui/color/include"
        COMPILE_OPTIONS -O1 -Wall -Wextra
    )
endif()

# --- Lexbor freestanding archive smoke test ---
if(EXISTS "${KYUZEN_UNIT_DIR}/lexbor_html_test.c")
    kyuzen_host_test(test-lexbor-host
        SOURCES "${KYUZEN_UNIT_DIR}/lexbor_html_test.c"
        INCLUDES
            "${_lexbor_dir}/kyuzen/include"
            "${_lexbor_dir}/source"
        COMPILE_OPTIONS -O1 -Wall -Wextra -Wno-unused-parameter
        LINK_LIBS kyuzen-host-lexbor
    )
endif()

# --- media: REAL libs/media/media.c as C + gallery thumbs as C++ ---
if(EXISTS "${KYUZEN_UNIT_DIR}/media_test.cpp")
    add_library(kyuzen-host-media OBJECT "${KYUZEN_ROOT}/libs/media/media.c")
    target_include_directories(kyuzen-host-media PRIVATE
        "${KYUZEN_ROOT}/include"
        "${KYUZEN_ROOT}/libs/gui/color/include"
    )
    target_compile_options(kyuzen-host-media PRIVATE -O1 -Wall -Wextra)

    kyuzen_host_test(test-media
        SOURCES
            "${KYUZEN_UNIT_DIR}/media_test.cpp"
            "${KYUZEN_ROOT}/apps/gallery/thumbs.cpp"
            $<TARGET_OBJECTS:kyuzen-host-media>
        IQUOTE "${KYUZEN_ROOT}/include" "${KYUZEN_ROOT}/libs/gui/color/include"
        INCLUDES "${KYUZEN_ROOT}/apps"
        COMPILE_OPTIONS -O1 -Wall -Wextra
    )
endif()

# --- KyuzenFS tree: REAL kernel/fs modules included directly ---
if(EXISTS "${KYUZEN_UNIT_DIR}/kyuzenfs_tree_test.c")
    kyuzen_host_test(test-kyuzenfs-tree
        SOURCES "${KYUZEN_UNIT_DIR}/kyuzenfs_tree_test.c"
        IQUOTE "${KYUZEN_ROOT}/test" "${KYUZEN_ROOT}/include"
        COMPILE_OPTIONS -O1 -Wall
    )
endif()

# --- libui toolkit: theme / XML / fileman widgets / owner propagation ---
# All four link the REAL toolkit sources and stub the syscall layer.
# --- libui toolkit: theme / XML / fileman widgets / owner propagation ---
set(_widget_srcs "")
kyuzen_glob(_widget_srcs "${KYUZEN_ROOT}/libs/gui/widget/src/*/*.cpp")
list(APPEND _widget_srcs "${KYUZEN_ROOT}/libs/gui/widget/abi/libui_abi.cpp")

foreach(_t IN ITEMS theme xml fileman_widgets owner)
    set(_src "${KYUZEN_UNIT_DIR}/libui_${_t}_test.cpp")
    if(EXISTS "${_src}")
        kyuzen_host_test(test-libui-${_t}
            SOURCES
                "${_src}"
                ${_widget_srcs}
                "${KYUZEN_ROOT}/libs/gui/color/src/color_utils.c"
            IQUOTE "${KYUZEN_ROOT}" "${KYUZEN_ROOT}/include"
            INCLUDES
                "${KYUZEN_ROOT}/libs/gui/widget/include"
                "${KYUZEN_ROOT}/libs/gui/color/include"
            # -std=c++17 applies to the C++ sources only: the target also
            # compiles color_utils.c, and clang rejects a C++ standard on a C
            # translation unit.
            COMPILE_OPTIONS
                "$<$<COMPILE_LANGUAGE:CXX>:-std=c++17>"
                -O1 -Wall
        )
        # Test XML memakai DOKUMEN SUNGGUHAN dari ui/xml/, bukan salinan
        # string di dalam test — supaya dokumen asli dan test tidak bisa
        # berbeda diam-diam (drift).
        if(_t STREQUAL "xml")
            kyuzen_embed_xml(test-libui-xml SOURCES xml_demo.xml)
        endif()
    endif()
endforeach()

# ---------------------------------------------------------------------------
# libui visual QA — MERENDER widget produksi ke PPM.
#
# Bukan test perilaku: ini alat untuk memeriksa KOMPOSISI (ritme jarak,
# kontras, state permukaan) pada 1280x720 dan 1920x1080, di mode terang dan
# gelap, tanpa boot QEMU. Keluarannya PPM di direktori kerja test; konversi ke
# PNG opsional di luar (bukan dependensi build).
#
# Didaftarkan sebagai test supaya ikut dijalankan `ctest`, tapi yang
# diperiksa hanyalah "render selesai tanpa crash / tanpa overflow" — nilai
# visualnya dinilai mata, bukan assertion.
# ---------------------------------------------------------------------------
if(EXISTS "${KYUZEN_UNIT_DIR}/libui_visual_qa.cpp")
    kyuzen_host_test(test-libui-visual-qa
        SOURCES
            "${KYUZEN_UNIT_DIR}/libui_visual_qa.cpp"
            ${_widget_srcs}
            "${KYUZEN_ROOT}/libs/gui/color/src/color_utils.c"
        IQUOTE "${KYUZEN_ROOT}" "${KYUZEN_ROOT}/include"
        INCLUDES
            "${KYUZEN_ROOT}/libs/gui/widget/include"
            "${KYUZEN_ROOT}/libs/gui/color/include"
        COMPILE_OPTIONS
            "$<$<COMPILE_LANGUAGE:CXX>:-std=c++17>"
            -O1 -Wall
    )
    set_tests_properties(test-libui-visual-qa PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/visual-qa")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/visual-qa")
endif()

# ---------------------------------------------------------------------------
# libui performance QA — ANGGARAN render (regresi guard).
#
# Mengukur berapa banyak pixel kanvas yang disentuh satu draw() widget, dalam
# satuan "layar 1280x720". Metrik ini stabil lintas mesin (bukan pengukuran
# waktu yang goyah) dan langsung menangkap regresi kelas besar: widget yang
# tiba-tiba menggambar seluruh layar, atau lapisan penuh yang ditambahkan ke
# jalur panas. KyuzenOS adalah OS tanpa akselerasi — biaya render adalah
# properti yang harus dijaga, bukan diasumsikan.
# ---------------------------------------------------------------------------
if(EXISTS "${KYUZEN_UNIT_DIR}/libui_perf_qa.cpp")
    kyuzen_host_test(test-libui-perf-qa
        SOURCES
            "${KYUZEN_UNIT_DIR}/libui_perf_qa.cpp"
            ${_widget_srcs}
            "${KYUZEN_ROOT}/libs/gui/color/src/color_utils.c"
        IQUOTE "${KYUZEN_ROOT}" "${KYUZEN_ROOT}/include"
        INCLUDES
            "${KYUZEN_ROOT}/libs/gui/widget/include"
            "${KYUZEN_ROOT}/libs/gui/color/include"
        COMPILE_OPTIONS
            "$<$<COMPILE_LANGUAGE:CXX>:-std=c++17>"
            -O1 -Wall
    )
endif()

# --- desktop framework: system/desktop modules + libdesktop backend ---
if(EXISTS "${KYUZEN_UNIT_DIR}/desktop_manifest_test.cpp")
    kyuzen_host_test(test-desktop
        SOURCES
            "${KYUZEN_UNIT_DIR}/desktop_manifest_test.cpp"
            "${KYUZEN_ROOT}/system/desktop/launcher.cpp"
            "${KYUZEN_ROOT}/system/desktop/crash_notice.cpp"
            "${KYUZEN_ROOT}/system/desktop/taskbar.cpp"
            "${KYUZEN_ROOT}/system/desktop/app_icons.cpp"
            "${KYUZEN_ROOT}/system/desktop/wallpaper.cpp"
            "${KYUZEN_ROOT}/system/desktop/app_preview.cpp"
            "${KYUZEN_ROOT}/system/desktop/desktop_shell.cpp"
            "${KYUZEN_ROOT}/libs/gui/libdesktop/src/event.cpp"
            "${KYUZEN_ROOT}/libs/gui/libdesktop/src/window_manager.cpp"
            "${KYUZEN_ROOT}/libs/gui/libdesktop/src/system.cpp"
            "${KYUZEN_ROOT}/libs/gui/libdesktop/src/canvas.cpp"
            "${KYUZEN_ROOT}/libs/text/src/kzfont.c"
            "${KYUZEN_ROOT}/libs/text/src/kzraster_ft.c"
        IQUOTE "${KYUZEN_ROOT}/include"
        INCLUDES
            "${KYUZEN_ROOT}/libs/text/include"
            "${KYUZEN_ROOT}/libs/gui/libdesktop/include"
            "${KYUZEN_ROOT}/system/desktop"
            "${KYUZEN_ROOT}/libs/gui/color/include"
        COMPILE_OPTIONS -O1 -Wall
    )
endif()

# --- LLVM libc heap: REAL freelist/freetrie with a mocked backing store ---
if(EXISTS "${KYUZEN_UNIT_DIR}/libc_heap_test.cpp")
    set(_libc_dir "${KYUZEN_ROOT}/third_party/stdlib/llvm-project/libc")
    if(EXISTS "${_libc_dir}/src/__support/freelist.cpp")
        kyuzen_host_test(test-libc-heap
            SOURCES
                "${KYUZEN_UNIT_DIR}/libc_heap_test.cpp"
                "${_libc_dir}/src/__support/freelist.cpp"
                "${_libc_dir}/src/__support/freetrie.cpp"
            INCLUDES
                "${_libc_dir}"
                "${KYUZEN_ROOT}/libs/c/libc-port/src"
            DEFINES
                LIBC_NAMESPACE=__llvm_libc_22_1_8_
                LIBC_FULL_BUILD
                LIBC_TARGET_OS_IS_BAREMETAL
                LIBC_ERRNO_MODE=LIBC_ERRNO_MODE_EXTERNAL
                LIBC_THREAD_MODE=LIBC_THREAD_MODE_SINGLE
                LIBC_COPT_PUBLIC_PACKAGING
            # -mno-sse2 keeps inline_memcpy from pulling <immintrin.h>, which
            # drags in mingw's <mm_malloc.h> -> <stdlib.h> and collides with
            # libc's own declarations.
            COMPILE_OPTIONS -std=gnu++17 -O1 -Wall -mno-sse -mno-sse2
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Known-failing tests, recorded so the baseline is explicit.
#
# Both fail in the Make build too (docs/development/build-system-audit.md §10.3). They are
# registered as DISABLED rather than removed: the failure stays visible in
# `ctest` output as "not run", and fixing the underlying source issue will make
# them pass without any build-system change.
#
#   test-kyuzenfs-tree  builds, then exits 0xC0000016 (STATUS_ILLEGAL_
#                       INSTRUCTION) with no output.
#   test-libc-heap      fails to COMPILE: LLVM libc's internal API drift
#                       (no member 'write_to_stderr' in the libc namespace).
#
# DISABLED alone is NOT enough for test-libc-heap. CTest's DISABLED property
# only skips the test at RUN time; Ninja still builds the target as part of
# `all`, the compile fails, and Ninja stops there — so ten unrelated test
# binaries downstream never linked and `ctest` reported them "Not Run" for no
# reason of their own. The target is therefore excluded from the default build
# as well. It can still be built on demand (`ninja test-libc-heap`) to watch
# the compile error, which is the point of keeping it.
#
# test-kyuzenfs-tree compiles cleanly and only fails at run time, so it stays
# in the default build — that way `ninja` still proves the test's own code
# compiles, which is real coverage even while the test is disabled.
# ---------------------------------------------------------------------------
if(TARGET test-libc-heap)
    set_tests_properties(test-libc-heap PROPERTIES DISABLED TRUE)
    set_target_properties(test-libc-heap PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
if(TARGET test-kyuzenfs-tree)
    set_tests_properties(test-kyuzenfs-tree PROPERTIES DISABLED TRUE)
endif()

# ---------------------------------------------------------------------------
# Host tool: KyuzenFS V4 disk formatter.
#
#   cmake --build build/host --target mkfs.kyuzenfs
#   ./mkfs.kyuzenfs disk.img
#
# This is a HOST program, not a bare-metal target: it formats a disk image on
# the build machine before boot. It therefore belongs in this build tree, not
# in the target tree — and it must NOT go through the global ld.lld link rule,
# which skips the platform startup code and C runtime.
#
# The Make build had a `mkfs` target for this. It was missed in the migration,
# so the tool was unreachable from CMake even though tools/mkfs.kyuzenfs.c was
# always present.
#
# NO INCLUDE DIRECTORY, deliberately — not even the repo's include/.
#
# The old recipe passed `-iquote include`, but the tool does not need it: it
# includes only <stdint.h>, <stdio.h>, <stdlib.h> and <string.h>, all with
# angle brackets, which -iquote would not resolve anyway. What it DOES need is
# the host's real <stdlib.h>, and the repo's include/stdlib.h is a freestanding
# shim that defines only `abs()` — no malloc, free or strtoull.
#
# Adding include/ as an include directory therefore breaks the tool outright:
#
#   mkfs.kyuzenfs.c:171:30: error: call to undeclared library function 'malloc'
#   mkfs.kyuzenfs.c:215:5:  error: call to undeclared library function 'free'
#   mkfs.kyuzenfs.c:104:48: error: call to undeclared library function 'strtoull'
#
# This is the same stdlib.h shadowing documented throughout this file, reached
# from the opposite direction: the tests need -iquote so the shim does NOT win,
# and this tool needs no path at all so the host header DOES.
# ---------------------------------------------------------------------------
if(EXISTS "${KYUZEN_ROOT}/tools/mkfs.kyuzenfs.c")
    add_executable(mkfs.kyuzenfs "${KYUZEN_ROOT}/tools/mkfs.kyuzenfs.c")
    target_compile_options(mkfs.kyuzenfs PRIVATE -O2 -Wall -Wextra)
    set_target_properties(mkfs.kyuzenfs PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
else()
    message(WARNING
        "mkfs.kyuzenfs: tools/mkfs.kyuzenfs.c is not in the repository, so the "
        "KyuzenFS disk formatter cannot be built.")
endif()

# ---------------------------------------------------------------------------
# Summary: report how many tests were registered so a silently-shrinking test
# suite is visible at configure time.
# ---------------------------------------------------------------------------
get_property(_all_tests DIRECTORY PROPERTY TESTS)
list(LENGTH _all_tests _test_count)
message(STATUS "Host tests registered: ${_test_count}")
