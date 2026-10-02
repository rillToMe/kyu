# ============================================================================
# apps/CMakeLists.txt — user-space applications.
#
# Four groups, each matching one recipe from the old build
# (docs/development/build-system-audit.md §6.2, §14.7). The grouping is by BUILD RECIPE, not by
# directory — that is how the old build organised them, and the recipes differ
# in flags, linker script, entry point, and link inputs.
#
#   A. GUI apps (apps/*.c)          C, ENTRY(main), toolkit + png + color
#   B. C++ apps (apps/<name>/*.cpp) C++, ENTRY(main), toolkit + png + media
#   C. Test ELFs (tests/target/*.c) C, ENTRY(main), userlib only
#   D. SDK apps (C++)               C++, ENTRY(_start), libc++ subset + CRT
#
# Every target is listed in KYUZEN_APP_TARGETS so the ISO can depend on the
# real targets instead of a phony umbrella (audit §13.3).
# ============================================================================

set(KYUZEN_APP_TARGETS "")

set(_apps_dir  "${KYUZEN_ROOT}/apps")
set(_tests_dir "${KYUZEN_ROOT}/tests/target")

# ===========================================================================
# Shared link inputs (mirroring GUI_LIBS / TERMINAL_LIBS / MEDIA_LIBS).
#
# ORDER IS EXACT — it is the order the old link lines used:
#   GUI_LIBS      = USERLIB NETUTIL LIBGUI WIDGET PNG COLOR_UTILS
#   TERMINAL_LIBS = USERLIB USERUTIL NETUTIL SHELLCORE LIBGUI WIDGET PNG COLOR_UTILS
#   MEDIA_LIBS    = GUI_LIBS + MEDIA
#
# NOTE: only color_utils.o is linked, not the whole libs/gui/color set — the
# old build compiled all three colour sources but linked one. See
# libs/gui/color/CMakeLists.txt.
# ===========================================================================
set(_gui_libs
    kyuzen-userlib
    kyuzen-netutil
    kyuzen-libgui
    kyuzen-widget
    kyuzen-png
    kyuzen-color-utils
)

set(_terminal_libs
    kyuzen-userlib
    kyuzen-userutil
    kyuzen-netutil
    kyuzen-shellcore
    kyuzen-libgui
    kyuzen-widget
    kyuzen-png
    kyuzen-color-utils
)

# ===========================================================================
# A. GUI apps — plain C, toolkit-linked.
# ===========================================================================
# Most GUI apps are compiled -O0 (CFLAGS_APP). Two exceptions:
#   viewer  -O2  (it decodes images; deliberately optimised)
#   clock        (uses the C SDK — built separately below)
foreach(_app calc notepad widget_demo xml_demo)
    kyuzen_add_plain_app(kyuzen-${_app}
        SOURCES "${_apps_dir}/${_app}.c"
        LIBS    ${_gui_libs}
    )
    list(APPEND KYUZEN_APP_TARGETS kyuzen-${_app})
endforeach()

# viewer is the one GUI app compiled with CFLAGS_LIB (-O2) because it decodes
# images. Using the app flag set (-O0) here changes the generated code and
# breaks binary parity.
kyuzen_add_plain_app(kyuzen-viewer
    SOURCES "${_apps_dir}/viewer.c"
    FLAGS   kyuzen-flags-applib
    LIBS    ${_gui_libs}
)
list(APPEND KYUZEN_APP_TARGETS kyuzen-viewer)

# ---------------------------------------------------------------------------
# clock — HYBRID: entry is main (apps/app.ld) but it calls snprintf/strlen from
# the C SDK. So it uses the SDK's CRT and libc.a while keeping ENTRY(main) and
# the toolkit link set.
#
# Compiled -O2 (the old rule used CFLAGS_LIB, not CFLAGS_APP) against the SDK
# headers. Archive order matters: libc.a goes LAST because a static archive
# only resolves symbols referenced by things already on the command line.
# ---------------------------------------------------------------------------
if(KYUZEN_BUILD_LIBC_SDK)
    kyuzen_add_plain_app(kyuzen-clock
        SOURCES "${_apps_dir}/clock.c"
        FLAGS   kyuzen-flags-applib
        LINKER_SCRIPT "${KYUZEN_SDK_C_LD}"
        LIBS
            ${_gui_libs}
            "${KYUZEN_SDK_C_CRT}"
            "${KYUZEN_SDK_C_LIB}"
    )
    # The app source compiles against the SDK headers, not third_party.
    add_dependencies(kyuzen-clock kyuzen-sdk-c)
    target_include_directories(kyuzen-clock-objects SYSTEM PRIVATE "${KYUZEN_SDK_C_INC}")
    list(APPEND KYUZEN_APP_TARGETS kyuzen-clock)
endif()

# ---------------------------------------------------------------------------
# fontdemo — toolkit + libs/text + the FreeType archive + embedded font blobs.
#
# The font TTF blobs are converted to objects with `ld -b binary`, giving
# symbols like _binary____assets_fonts_DejaVuSans_ttf_start. No hand-written
# byte arrays.
# ---------------------------------------------------------------------------
set(_fontdemo_fonts
    DejaVuSans
    Inter-Regular
    NotoSansAdlam-Regular
    NotoSansMono-Regular
    NotoSansMono-Bold
)

# ---------------------------------------------------------------------------
# Font blobs: TTF -> object via `ld -r --format=binary`.
#
# SYMBOL NAMES DERIVE FROM THE INPUT PATH. The old recipe ran from apps/ and
# passed `../assets/fonts/<Name>.ttf`, producing symbols like
#     _binary____assets_fonts_DejaVuSans_ttf_start
# (note the four underscores: two from the `..` prefix, two from the leading
# slash). fontdemo.c references those exact names, so the relative path and
# working directory must match — an absolute path yields a completely
# different symbol.
# ---------------------------------------------------------------------------
set(_font_blob_objs "")
foreach(_font IN LISTS _fontdemo_fonts)
    set(_ttf_rel "../assets/fonts/${_font}.ttf")
    set(_obj "${CMAKE_BINARY_DIR}/obj/user/apps/fonts/${_font}.o")

    add_custom_command(
        OUTPUT "${_obj}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/obj/user/apps/fonts"
        COMMAND ${KYUZEN_LD_LLD} -m elf_x86_64 -r --format=binary -o "${_obj}" "${_ttf_rel}"
        DEPENDS "${KYUZEN_ROOT}/assets/fonts/${_font}.ttf"
        WORKING_DIRECTORY "${_apps_dir}"
        COMMENT "Embedding font blob: ${_font}.ttf"
        VERBATIM
    )
    list(APPEND _font_blob_objs "${_obj}")
endforeach()

# The blobs are raw objects attached via target_link_libraries, so nothing in
# the graph would otherwise depend on the custom commands above and Ninja
# reports "missing and no known rule to make it". This target provides the
# producer edge.
add_custom_target(kyuzen-font-blobs DEPENDS ${_font_blob_objs})

kyuzen_add_plain_app(kyuzen-fontdemo
    SOURCES "${_apps_dir}/fontdemo.c"
    LIBS
        ${_gui_libs}
        kyuzen-text-manager
        kyuzen-text-raster
        kyuzen-freetype
        ${_font_blob_objs}
    INCLUDES "${KYUZEN_ROOT}/libs/text/include"
)
add_dependencies(kyuzen-fontdemo kyuzen-font-blobs)
list(APPEND KYUZEN_APP_TARGETS kyuzen-fontdemo)

# ===========================================================================
# B. terminal — plain C but with the shell engine, so it needs its own link set.
# ===========================================================================
kyuzen_add_plain_app(kyuzen-terminal
    SOURCES "${_apps_dir}/terminal.c"
    LIBS    ${_terminal_libs}
)
list(APPEND KYUZEN_APP_TARGETS kyuzen-terminal)

# ===========================================================================
# C. C++ apps: gallery, imageview.
#
# Compiled with the widget flag set (CXXFLAGS_LIB: -fno-exceptions -fno-rtti
# -std=c++17) and linked with apps/app.ld — ENTRY(main), no .init_array walk.
# Therefore they must NOT use global constructors; objects are created inside
# main(). libs/media is added on top of the GUI libs.
#
# The app's sources and the toolkit objects both go through
# kyuzen_expand_objects so INTERFACE wrappers (kyuzen-widget) resolve to their
# underlying OBJECT libraries, and the whole ordered sequence lands in
# target_link_options where CMake preserves it.
# ===========================================================================
function(_kyuzen_add_cpp_toolkit_app name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})

    add_library(${name}-objects OBJECT ${ARG_SOURCES})
    target_link_libraries(${name}-objects PRIVATE kyuzen-flags-widget)
    target_include_directories(${name}-objects PRIVATE
        "${KYUZEN_ROOT}/libs/gui/widget/include"
    )

    kyuzen_expand_objects(_lib_items ${_gui_libs} kyuzen-media)

    kyuzen_placeholder_object(_placeholder)
    add_executable(kyuzen-${name} "${_placeholder}")
    set_source_files_properties("${_placeholder}" PROPERTIES
        EXTERNAL_OBJECT TRUE GENERATED TRUE)
    set_target_properties(kyuzen-${name} PROPERTIES LINKER_LANGUAGE C)

    target_link_options(kyuzen-${name} PRIVATE
        -m elf_x86_64
        -nostdlib
        -T ${KYUZEN_ROOT}/apps/app.ld
        $<TARGET_OBJECTS:${name}-objects>
        ${_lib_items}
    )
    add_dependencies(kyuzen-${name} ${name}-objects)

    # Build ordering for the toolkit objects, which reach the link line through
    # the generator expressions above (CMake does not infer those edges).
    kyuzen_object_libraries(_obj_libs ${_gui_libs} kyuzen-media)
    foreach(_ol IN LISTS _obj_libs)
        add_dependencies(kyuzen-${name} ${_ol})
    endforeach()

    kyuzen_output_basename(kyuzen-${name} _out_name)
    set_target_properties(kyuzen-${name} PROPERTIES
        OUTPUT_NAME "${_out_name}"
        SUFFIX ".elf"
        RUNTIME_OUTPUT_DIRECTORY "${KYUZEN_ELF_DIR}"
        LINK_DEPENDS "${KYUZEN_ROOT}/apps/app.ld"
    )
endfunction()

_kyuzen_add_cpp_toolkit_app(gallery
    SOURCES
        "${_apps_dir}/gallery/main.cpp"
        "${_apps_dir}/gallery/gallery.cpp"
        "${_apps_dir}/gallery/thumbs.cpp"
)
list(APPEND KYUZEN_APP_TARGETS kyuzen-gallery)

_kyuzen_add_cpp_toolkit_app(imageview
    SOURCES
        "${_apps_dir}/imageview/main.cpp"
        "${_apps_dir}/imageview/viewer.cpp"
        "${_apps_dir}/imageview/image.cpp"
)
list(APPEND KYUZEN_APP_TARGETS kyuzen-imageview)

# ===========================================================================
# D. Test ELFs — run inside QEMU, link userlib only.
# ===========================================================================
foreach(_app badptr exit_test kill_test fd_test pipe_test fork_test procinfo)
    kyuzen_add_plain_app(kyuzen-${_app}
        SOURCES "${_tests_dir}/${_app}.c"
        LIBS    kyuzen-userlib
    )
    list(APPEND KYUZEN_APP_TARGETS kyuzen-${_app})
endforeach()

# ===========================================================================
# E. SDK apps — C++ with the libc++ subset, CRT, and .init_array support.
#
# These are the "real" applications: they link the staged C++ SDK (which brings
# libc.a, the CRT, and libcxxrt.a) and may use global constructors because the
# SDK linker script walks .init_array.
#
# The toolkit objects (widget/color/media/text/libgui/userlib) are linked in
# as OBJECT libraries, so they are included whole — the same as the old
# Makefile, which passed them as explicit object files.
# ===========================================================================
if(KYUZEN_BUILD_LIBC_SDK)

# --- desktop -----------------------------------------------------------------
# The implementation directory is selectable (KYUZEN_DESKTOP_APP), matching the
# old DESKTOP_APP variable. The output name is always desktop.elf so that
# limine.conf, the manifest, and login's spawn path never change.
set(_desktop_impl_dir "${KYUZEN_ROOT}/system/${KYUZEN_DESKTOP_APP}")

if(NOT IS_DIRECTORY "${_desktop_impl_dir}")
    message(FATAL_ERROR
        "desktop: implementation directory does not exist: ${_desktop_impl_dir}\n"
        "Set -DKYUZEN_DESKTOP_APP=<name> to one of the directories under system/.")
endif()

kyuzen_glob(_desktop_sources "${_desktop_impl_dir}/*.cpp")
if(NOT _desktop_sources)
    message(FATAL_ERROR
        "desktop: no *.cpp sources in ${_desktop_impl_dir}")
endif()

kyuzen_add_sdk_app(kyuzen-desktop
    SOURCES ${_desktop_sources}
    LIBS
        kyuzen-libdesktop
        kyuzen-userlib
        kyuzen-libgui
        kyuzen-netutil
        kyuzen-png
        kyuzen-text-manager
        kyuzen-text-raster
        kyuzen-freetype
    INCLUDES
        "${KYUZEN_ROOT}/libs/gui/libdesktop/include"
        "${KYUZEN_ROOT}/system/desktop"
        "${KYUZEN_ROOT}/libs/text/include"
)
# Output must be exactly desktop.elf regardless of which implementation fills it.
set_target_properties(kyuzen-desktop PROPERTIES OUTPUT_NAME "desktop")
list(APPEND KYUZEN_APP_TARGETS kyuzen-desktop)

# --- fileman -----------------------------------------------------------------
# Link order from the old recipe. The recipe is
#     $(FM_OBJS) $(USERAPP_LIB_OBJS) $$(ls $(FM_TOOLKIT_GLOBS))
# and `ls` SORTS its arguments, so the glob list is expanded alphabetically
# rather than in the order the patterns are written:
#     color/src/color_utils.o, widget/abi/libui_abi.o,
#     widget/src/chrome/*.o ... widget/src/xml/*.o, media.o
# Reproducing that sorted order is what places the colour objects at the same
# addresses as the Make build.
kyuzen_glob(_fileman_sources "${_apps_dir}/filemanager/*.cpp")
kyuzen_add_sdk_app(kyuzen-fileman
    SOURCES ${_fileman_sources}
    LIBS
        kyuzen-userlib
        kyuzen-libgui
        kyuzen-netutil
        kyuzen-png
        kyuzen-color-utils
        kyuzen-widget-abi
        kyuzen-widget-src
        kyuzen-media
    INCLUDES
        "${KYUZEN_ROOT}/libs/gui/widget/include"
)
set_target_properties(kyuzen-fileman PROPERTIES OUTPUT_NAME "fileman")
list(APPEND KYUZEN_APP_TARGETS kyuzen-fileman)

# --- settings ----------------------------------------------------------------
# Order: ST_OBJS, USERAPP_LIB_OBJS, TEXT_USER_OBJS, FT archive, then the same
# `ls`-sorted toolkit globs (colour first, then widget).
kyuzen_glob(_settings_sources "${_apps_dir}/settings/*.cpp")
kyuzen_add_sdk_app(kyuzen-settings
    SOURCES ${_settings_sources}
    LIBS
        kyuzen-userlib
        kyuzen-libgui
        kyuzen-netutil
        kyuzen-png
        kyuzen-text-manager
        kyuzen-text-raster
        kyuzen-freetype
        kyuzen-color-utils
        kyuzen-widget-abi
        kyuzen-widget-src
    INCLUDES "${KYUZEN_ROOT}/libs/text/include"
)
set_target_properties(kyuzen-settings PROPERTIES OUTPUT_NAME "settings")
list(APPEND KYUZEN_APP_TARGETS kyuzen-settings)

# --- taskmgr -----------------------------------------------------------------
# Order: TM_OBJS, USERAPP_LIB_OBJS, then the `ls`-sorted toolkit globs.
kyuzen_glob(_taskmgr_sources "${_apps_dir}/taskmgr/*.cpp")
kyuzen_add_sdk_app(kyuzen-taskmgr
    SOURCES ${_taskmgr_sources}
    LIBS
        kyuzen-userlib
        kyuzen-libgui
        kyuzen-netutil
        kyuzen-png
        kyuzen-color-utils
        kyuzen-widget-abi
        kyuzen-widget-src
)
set_target_properties(kyuzen-taskmgr PROPERTIES OUTPUT_NAME "taskmgr")
list(APPEND KYUZEN_APP_TARGETS kyuzen-taskmgr)

# --- browser -----------------------------------------------------------------
# The heaviest app: C++ toolkit + libtext/FreeType + Lexbor + the BearSSL OS
# subset (objects, not an archive).
#
# NOTE: the old Makefile's comment mentions "one C TU (imgdec.c: stb_image from
# memory)". That file does not exist in the tree — stb_image is compiled inside
# libs/media/png.c, which the browser already links via kyuzen-png. The comment
# is stale; no separate C TU is needed here.
# ---------------------------------------------------------------------------
# Browser source order.
#
# The old recipe used TWO separate wildcards:
#     $(wildcard $(BROWSER_DIR)/*.cpp) $(wildcard $(BROWSER_DIR)/engine/*.cpp)
# Each wildcard is sorted independently and the results are CONCATENATED, so the
# order is:
#     browser_app, font, main, page, transport, transport_tls,
#     css, html, http, layout, lexbor_html_adapter, url
# NOT one globally sorted list. Object order determines symbol addresses.
#
# The list is written out EXPLICITLY rather than globbed: CMake sorts the
# sources of a target by absolute path, which would interleave the engine/
# directory with the root files and change every symbol address.
# ---------------------------------------------------------------------------
set(_browser_cpp
    "${_apps_dir}/browser/browser_app.cpp"
    "${_apps_dir}/browser/font.cpp"
    "${_apps_dir}/browser/main.cpp"
    "${_apps_dir}/browser/page.cpp"
    "${_apps_dir}/browser/transport.cpp"
    "${_apps_dir}/browser/transport_tls.cpp"
    "${_apps_dir}/browser/engine/css.cpp"
    "${_apps_dir}/browser/engine/html.cpp"
    "${_apps_dir}/browser/engine/http.cpp"
    "${_apps_dir}/browser/engine/layout.cpp"
    "${_apps_dir}/browser/engine/lexbor_html_adapter.cpp"
    "${_apps_dir}/browser/engine/url.cpp"
)

kyuzen_add_sdk_app(kyuzen-browser
    SOURCES ${_browser_cpp}
    LIBS
        kyuzen-bearssl-os
        kyuzen-userlib
        kyuzen-libgui
        kyuzen-netutil
        kyuzen-png
        kyuzen-text-manager
        kyuzen-text-raster
        kyuzen-freetype
        kyuzen-lexbor
        kyuzen-color-utils
        kyuzen-widget-abi
        kyuzen-widget-src
    INCLUDES
        "${KYUZEN_ROOT}/apps/browser"
        "${KYUZEN_ROOT}/libs/text/include"
        "${KYUZEN_ROOT}/third_party/lexbor/kyuzen/include"
        "${KYUZEN_ROOT}/third_party/lexbor/source"
)
# The BearSSL OS objects and the generated trust anchors must exist before the
# browser links. tls_kyuzen.c #includes tas_https.inc, so the anchors are also
# needed before the browser's own sources compile.
#
# The dependency is declared on BOTH targets: the objects library needs the
# anchors to compile, and the executable needs the objects to link (they reach
# the link line through $<TARGET_OBJECTS:...> inside target_link_options, which
# CMake does not treat as a dependency).
add_dependencies(kyuzen-browser-objects kyuzen-bearssl-tas)
add_dependencies(kyuzen-browser kyuzen-bearssl-os kyuzen-bearssl-tas)
set_target_properties(kyuzen-browser PROPERTIES OUTPUT_NAME "browser")
target_compile_options(kyuzen-browser PRIVATE
    -iquote "${KYUZEN_ROOT}/include"
    -DLEXBOR_STATIC
)
list(APPEND KYUZEN_APP_TARGETS kyuzen-browser)

endif() # KYUZEN_BUILD_LIBC_SDK

# ===========================================================================
# Post-build verification for every SDK-built ELF.
#
# These are the guards the old Makefile ran on desktop/fileman/settings/taskmgr/
# browser: entry point, zero undefined symbols, no exception/thread runtime, no
# SSE or x87 instructions.
#
# The SDK artifacts (libcxxrt.a, cxxrt.o, crt.o, libc.a) and the staging
# dependency are wired up by kyuzen_add_sdk_app itself.
# ===========================================================================
if(KYUZEN_BUILD_LIBC_SDK)
    foreach(_t kyuzen-desktop kyuzen-fileman kyuzen-settings kyuzen-taskmgr kyuzen-browser)
        if(TARGET ${_t})
            kyuzen_verify_user_elf(${_t})
        endif()
    endforeach()
endif()

# ===========================================================================
# Expose the app target list to the ISO module.
# ===========================================================================
set(KYUZEN_APP_TARGETS_FROM_APPS "${KYUZEN_APP_TARGETS}" PARENT_SCOPE)
