# ============================================================================
# apps/CMakeLists.txt — user-space applications.
#
# Four groups, each matching one recipe from the old build
# (BUILD_SYSTEM_AUDIT.md §6.2, §14.7). The grouping is by BUILD RECIPE, not by
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
# Archive order matters: libc.a goes LAST because a static archive only
# resolves symbols referenced by things already on the command line.
# ---------------------------------------------------------------------------
if(KYUZEN_BUILD_LIBC_SDK)
    kyuzen_add_plain_app(kyuzen-clock
        SOURCES "${_apps_dir}/clock.c"
        LIBS
            ${_gui_libs}
            "${KYUZEN_SDK_C_CRT}"
            "${KYUZEN_SDK_C_LIB}"
    )
    # The app source compiles against the SDK headers, not third_party.
    add_dependencies(kyuzen-clock kyuzen-sdk-c)
    target_include_directories(kyuzen-clock SYSTEM PRIVATE "${KYUZEN_SDK_C_INC}")
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

set(_font_blob_objs "")
foreach(_font IN LISTS _fontdemo_fonts)
    set(_ttf "${KYUZEN_ROOT}/assets/fonts/${_font}.ttf")
    set(_obj "${CMAKE_BINARY_DIR}/obj/user/apps/fonts/${_font}.o")

    add_custom_command(
        OUTPUT "${_obj}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/obj/user/apps/fonts"
        COMMAND ${KYUZEN_LD_LLD} -m elf_x86_64 -r --format=binary -o "${_obj}" "${_ttf}"
        DEPENDS "${_ttf}"
        COMMENT "Embedding font blob: ${_font}.ttf"
        VERBATIM
    )
    list(APPEND _font_blob_objs "${_obj}")
endforeach()

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
# ===========================================================================
function(_kyuzen_add_cpp_toolkit_app name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})

    add_executable(kyuzen-${name} ${ARG_SOURCES})
    target_link_libraries(kyuzen-${name} PRIVATE
        kyuzen-flags-widget
        ${_gui_libs}
        kyuzen-media
    )
    target_link_options(kyuzen-${name} PRIVATE
        -m elf_x86_64 -nostdlib -T ${KYUZEN_ROOT}/apps/app.ld
    )
    target_include_directories(kyuzen-${name} PRIVATE
        "${KYUZEN_ROOT}/libs/gui/widget/include"
    )
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

file(GLOB _desktop_sources CONFIGURE_DEPENDS "${_desktop_impl_dir}/*.cpp")
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
target_compile_options(kyuzen-desktop PRIVATE -iquote "${KYUZEN_ROOT}/include")
list(APPEND KYUZEN_APP_TARGETS kyuzen-desktop)

# --- fileman -----------------------------------------------------------------
file(GLOB _fileman_sources CONFIGURE_DEPENDS "${_apps_dir}/filemanager/*.cpp")
kyuzen_add_sdk_app(kyuzen-fileman
    SOURCES ${_fileman_sources}
    LIBS
        kyuzen-userlib kyuzen-libgui kyuzen-widget kyuzen-png kyuzen-media kyuzen-color
    INCLUDES
        "${KYUZEN_ROOT}/libs/gui/widget/include"
)
set_target_properties(kyuzen-fileman PROPERTIES OUTPUT_NAME "fileman")
target_compile_options(kyuzen-fileman PRIVATE -iquote "${KYUZEN_ROOT}/include")
list(APPEND KYUZEN_APP_TARGETS kyuzen-fileman)

# --- settings ----------------------------------------------------------------
file(GLOB _settings_sources CONFIGURE_DEPENDS "${_apps_dir}/settings/*.cpp")
kyuzen_add_sdk_app(kyuzen-settings
    SOURCES ${_settings_sources}
    LIBS
        kyuzen-userlib kyuzen-libgui kyuzen-widget kyuzen-png kyuzen-color
        kyuzen-text-manager kyuzen-text-raster kyuzen-freetype
    INCLUDES "${KYUZEN_ROOT}/libs/text/include"
)
set_target_properties(kyuzen-settings PROPERTIES OUTPUT_NAME "settings")
target_compile_options(kyuzen-settings PRIVATE -iquote "${KYUZEN_ROOT}/include")
list(APPEND KYUZEN_APP_TARGETS kyuzen-settings)

# --- taskmgr -----------------------------------------------------------------
file(GLOB _taskmgr_sources CONFIGURE_DEPENDS "${_apps_dir}/taskmgr/*.cpp")
kyuzen_add_sdk_app(kyuzen-taskmgr
    SOURCES ${_taskmgr_sources}
    LIBS kyuzen-userlib kyuzen-libgui kyuzen-widget kyuzen-png kyuzen-color
)
set_target_properties(kyuzen-taskmgr PROPERTIES OUTPUT_NAME "taskmgr")
target_compile_options(kyuzen-taskmgr PRIVATE -iquote "${KYUZEN_ROOT}/include")
list(APPEND KYUZEN_APP_TARGETS kyuzen-taskmgr)

# --- browser -----------------------------------------------------------------
# The heaviest app: C++ toolkit + libtext/FreeType + Lexbor + the BearSSL OS
# subset (objects, not an archive).
#
# NOTE: the old Makefile's comment mentions "one C TU (imgdec.c: stb_image from
# memory)". That file does not exist in the tree — stb_image is compiled inside
# libs/media/png.c, which the browser already links via kyuzen-png. The comment
# is stale; no separate C TU is needed here.
file(GLOB _browser_cpp CONFIGURE_DEPENDS
    "${_apps_dir}/browser/*.cpp"
    "${_apps_dir}/browser/engine/*.cpp"
)

kyuzen_add_sdk_app(kyuzen-browser
    SOURCES ${_browser_cpp}
    LIBS
        $<TARGET_OBJECTS:kyuzen-bearssl-os>
        kyuzen-userlib kyuzen-libgui kyuzen-widget kyuzen-png kyuzen-color
        kyuzen-text-manager kyuzen-text-raster kyuzen-freetype
        kyuzen-lexbor
    INCLUDES
        "${KYUZEN_ROOT}/apps/browser"
        "${KYUZEN_ROOT}/libs/text/include"
        "${KYUZEN_ROOT}/third_party/lexbor/kyuzen/include"
        "${KYUZEN_ROOT}/third_party/lexbor/source"
)
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
# ===========================================================================
if(KYUZEN_BUILD_LIBC_SDK)
    foreach(_t kyuzen-desktop kyuzen-fileman kyuzen-settings kyuzen-taskmgr kyuzen-browser)
        if(TARGET ${_t})
            kyuzen_verify_user_elf(${_t})
        endif()
    endforeach()
endif()

# ===========================================================================
# All SDK apps consume the staged C++ SDK: the header closure, libcxxrt.a,
# cxxrt.o, the CRT, libc.a and the linker script. Declaring the dependency
# once here means a stale SDK can never be linked by accident.
# ===========================================================================
if(KYUZEN_BUILD_LIBC_SDK)
    foreach(_t kyuzen-desktop kyuzen-fileman kyuzen-settings kyuzen-taskmgr kyuzen-browser)
        if(TARGET ${_t})
            add_dependencies(${_t} kyuzen-sdk-cpp)
            target_link_libraries(${_t} PRIVATE
                "${KYUZEN_SDK_CPP_CXXRT}"
                "${KYUZEN_SDK_C_CRT}"
                "${KYUZEN_SDK_C_LIB}"
            )
        endif()
    endforeach()
endif()

# ===========================================================================
# Expose the app target list to the ISO module.
# ===========================================================================
set(KYUZEN_APP_TARGETS_FROM_APPS "${KYUZEN_APP_TARGETS}" PARENT_SCOPE)
