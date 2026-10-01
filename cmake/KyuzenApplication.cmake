# ============================================================================
# KyuzenApplication.cmake — helper functions for user-space ELF targets.
#
# The old build had THREE different application recipes spread across the root
# Makefile and apps/Makefile (BUILD_SYSTEM_AUDIT.md §6.2, §14.7). Each helper
# below reproduces exactly one of them, including its linker script, entry
# point, flag set, and link order.
#
#   kyuzen_add_plain_app()  <- apps/Makefile SYS_APPS / TEST_APPS rule
#   kyuzen_add_gui_app()    <- apps/Makefile GUI_APPS / OTHER_APPS / CPP_APPS
#   kyuzen_add_sdk_app()    <- root Makefile C++ SDK apps (desktop/fileman/...)
#
# LINK ORDER IS LOAD-BEARING. Static archives resolve left-to-right: a symbol
# referenced by an earlier object is only found if the archive defining it
# appears later on the command line. CMake preserves the order given to
# target_link_libraries, so the order below is deliberate, not alphabetical.
# ============================================================================

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# kyuzen_output_basename(<target> <out_var>)
#
# Target names carry a `kyuzen-` prefix to avoid colliding with anything else
# in the tree, but the ELF on disk must keep the bare name the rest of the
# system expects: limine.conf lists `echo.elf`, manifests/*.app name it, and
# the shell spawns it by that name. So the prefix is stripped for OUTPUT_NAME.
# ---------------------------------------------------------------------------
function(kyuzen_output_basename target out_var)
    string(REGEX REPLACE "^kyuzen-" "" _base "${target}")
    set(${out_var} "${_base}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kyuzen_placeholder_object(<out_var>)
#
# An empty object used as the sole nominal "source" of executables whose real
# content arrives as $<TARGET_OBJECTS:...>. add_executable requires at least
# one source, and the placeholder must contribute NOTHING to the linked image.
#
# Two details matter for byte parity:
#   * No symbols. A C TU emits STT_FILE, and nasm always emits one too, so the
#     source is empty and assembled by clang.
#   * No .text section at all. An empty .s still produces an EMPTY .text with
#     alignment 4, and the linker pads the combined .text with int3 bytes to
#     satisfy that alignment — adding one byte to the image. Stripping the
#     object with llvm-objcopy --remove-section=.text removes the section (and
#     its alignment) entirely, so the placeholder contributes nothing.
# ---------------------------------------------------------------------------
function(kyuzen_placeholder_object out_var)
    set(_src "${CMAKE_BINARY_DIR}/obj/kyuzen-placeholder.s")
    set(_obj "${CMAKE_BINARY_DIR}/obj/kyuzen-placeholder.o")

    if(NOT EXISTS "${_src}")
        file(WRITE "${_src}" "")
    endif()

    if(NOT TARGET kyuzen-placeholder)
        add_custom_command(
            OUTPUT "${_obj}"
            COMMAND "${KYUZEN_CLANG}" --target=x86_64-pc-none-elf -c "${_src}" -o "${_obj}"
            COMMAND "${KYUZEN_LLVM_OBJCOPY}" --remove-section=.text "${_obj}"
            DEPENDS "${_src}"
            COMMENT "Creating empty placeholder object"
            VERBATIM
        )
        add_custom_target(kyuzen-placeholder DEPENDS "${_obj}")
    endif()

    set(${out_var} "${_obj}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kyuzen_expand_objects(<out_var> <target-or-path>...)
#
# Recursively expand a mixed list into ordered object items:
#
#   OBJECT library  -> $<TARGET_OBJECTS:name>
#   INTERFACE lib   -> recurse into its INTERFACE_LINK_LIBRARIES
#   file path       -> unchanged
#
# The recursion matters because a convenience INTERFACE target (kyuzen-widget)
# names two OBJECT libraries, and $<TARGET_OBJECTS:...> cannot be applied to an
# INTERFACE library — its members would silently vanish from the link line and
# the app would fail with undefined ui_* symbols.
# ---------------------------------------------------------------------------
function(kyuzen_expand_objects out_var)
    set(_items "")
    foreach(_lib IN LISTS ARGN)
        if(TARGET ${_lib})
            get_target_property(_type ${_lib} TYPE)
            if(_type STREQUAL "OBJECT_LIBRARY")
                list(APPEND _items "$<TARGET_OBJECTS:${_lib}>")
            elseif(_type STREQUAL "INTERFACE_LIBRARY")
                get_target_property(_deps ${_lib} INTERFACE_LINK_LIBRARIES)
                if(_deps)
                    kyuzen_expand_objects(_sub ${_deps})
                    list(APPEND _items ${_sub})
                endif()
            else()
                list(APPEND _items "${_lib}")
            endif()
        else()
            list(APPEND _items "${_lib}")
        endif()
    endforeach()
    set(${out_var} "${_items}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kyuzen_link_items(<out_var> <target-or-path>...)
#
# Normalise a mixed list of OBJECT libraries and plain file paths into link
# items that CMake places IN THE ORDER GIVEN. INTERFACE libraries are expanded
# recursively (see above).
# ---------------------------------------------------------------------------
function(kyuzen_link_items out_var)
    kyuzen_expand_objects(_items ${ARGN})
    set(${out_var} "${_items}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kyuzen_add_plain_app(<name>
#     SOURCES   <src>...
#     [FLAGS    <flag-set-target>]        default: kyuzen-flags-app
#     [LINKER_SCRIPT <path>]              default: apps/app.ld
#     [LIBS     <target-or-file>...]
#     [INCLUDES <dir>...]
#     [COMPILE_OPTIONS <opt>...]
#     [LINK_OPTIONS    <opt>...]
#     [DEFINES  <def>...]
# )
#
# Simple freestanding C app: links userlib plus whatever extra libraries the
# app declares.
#
# FLAGS selects the canonical flag set. The old apps/Makefile used TWO sets for
# this group of targets:
#   CFLAGS_APP (-O0)  apps/*.c            — application code
#   CFLAGS_LIB (-O2)  system/*.c, viewer, clock — library-style code
# Passing the wrong one changes generated code and breaks binary parity.
#
# NOTE: passing `-O2` via COMPILE_OPTIONS does NOT work — CMake places a
# target's own options BEFORE those inherited from an INTERFACE library, and
# clang honours the LAST -O, so the interface's -O0 would win. The flag set
# itself must be the right one.
#
# LINKER_SCRIPT selects the entry point:
#   apps/app.ld          ENTRY(main)    — no CRT, no .init_array walk
#   libs/c/linker/app.ld ENTRY(_start)  — SDK CRT, used by apps that link libc
# Using the wrong script leaves every section and symbol identical but sets the
# ELF entry to the image base instead of _start.
# ---------------------------------------------------------------------------
function(kyuzen_add_plain_app name)
    cmake_parse_arguments(ARG
        ""
        "FLAGS;LINKER_SCRIPT"
        "SOURCES;LIBS;INCLUDES;COMPILE_OPTIONS;LINK_OPTIONS;DEFINES"
        ${ARGN}
    )

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "kyuzen_add_plain_app(${name}): SOURCES is required")
    endif()

    if(NOT ARG_FLAGS)
        set(ARG_FLAGS kyuzen-flags-app)
    endif()

    if(NOT ARG_LINKER_SCRIPT)
        set(ARG_LINKER_SCRIPT "${KYUZEN_ROOT}/apps/app.ld")
    endif()

    # The app's own sources become an OBJECT library so they can be placed at
    # a controlled position in the link line (see below).
    add_library(${name}-objects OBJECT ${ARG_SOURCES})
    target_link_libraries(${name}-objects PRIVATE ${ARG_FLAGS})
    if(ARG_INCLUDES)
        target_include_directories(${name}-objects PRIVATE ${ARG_INCLUDES})
    endif()
    if(ARG_DEFINES)
        target_compile_definitions(${name}-objects PRIVATE ${ARG_DEFINES})
    endif()
    if(ARG_COMPILE_OPTIONS)
        target_compile_options(${name}-objects PRIVATE ${ARG_COMPILE_OPTIONS})
    endif()

    kyuzen_placeholder_object(_placeholder)
    add_executable(${name} "${_placeholder}")
    set_source_files_properties("${_placeholder}" PROPERTIES
        EXTERNAL_OBJECT TRUE GENERATED TRUE)
    set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)

    kyuzen_link_items(_lib_items ${ARG_LIBS})

    # -----------------------------------------------------------------------
    # LINK ORDER — the app's own objects FIRST, then the libraries.
    #
    # The old link line was:
    #     ld.lld ... $(APP_OBJECT) $(LIBRARY_OBJECTS) -o $@
    # CMake emits OBJECT libraries before the target's own objects, which
    # reorders the sections the linker lays down and changes .text by a few
    # bytes. $<TARGET_OBJECTS:...> is the one form CMake places exactly where
    # it appears in the list, so both halves are passed that way.
    # -----------------------------------------------------------------------
    target_link_libraries(${name} PRIVATE
        $<TARGET_OBJECTS:${name}-objects>
        ${_lib_items}
    )

    target_link_options(${name} PRIVATE
        -m elf_x86_64
        -nostdlib
        -T ${ARG_LINKER_SCRIPT}
    )

    kyuzen_output_basename(${name} _out_name)
    set_target_properties(${name} PROPERTIES
        OUTPUT_NAME "${_out_name}"
        SUFFIX ".elf"
        RUNTIME_OUTPUT_DIRECTORY "${KYUZEN_ELF_DIR}"
        LINK_DEPENDS "${ARG_LINKER_SCRIPT}"
    )

    if(ARG_LINK_OPTIONS)
        target_link_options(${name} PRIVATE ${ARG_LINK_OPTIONS})
    endif()
endfunction()

# ---------------------------------------------------------------------------
# kyuzen_add_sdk_app(<name>
#     SOURCES <src>...
#     [LIBS   <target-or-file>...]
#     [INCLUDES ...] [COMPILE_OPTIONS ...] [DEFINES ...]
# )
#
# C++ SDK app: compiled with the canonical C++ SDK flags and linked with the
# SDK's own linker script (ENTRY(_start), which DOES walk .init_array — this
# is what makes global constructors work for these apps but not for the
# toolkit-only apps).
#
# Canonical link order reproduced from libs/cpp/bin/kyuzen-c++:
#     app objects -> libcxxrt.a -> cxxrt.o -> crt.o -> libc.a
# and the app's own extra libraries go BEFORE the runtime archives.
# ---------------------------------------------------------------------------
function(kyuzen_add_sdk_app name)
    cmake_parse_arguments(ARG
        ""
        ""
        "SOURCES;LIBS;INCLUDES;COMPILE_OPTIONS;DEFINES"
        ${ARGN}
    )

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "kyuzen_add_sdk_app(${name}): SOURCES is required")
    endif()

    # The app's own sources become an OBJECT library so they can be placed at
    # a controlled position in the link line (see kyuzen_add_plain_app).
    add_library(${name}-objects OBJECT ${ARG_SOURCES})
    target_link_libraries(${name}-objects PRIVATE kyuzen-flags-sdk-cpp)

    # The staged SDK headers are SYSTEM includes: they are searched after the
    # project's own headers, the same relationship the old kyuzen-c++ wrapper
    # established with -isystem.
    target_include_directories(${name}-objects SYSTEM PRIVATE
        "${KYUZEN_SDK_CPP_INC}"
        "${KYUZEN_SDK_C_INC}"
    )
    # The repo's include/ must be reached with -iquote, NOT -I. It contains a
    # stdlib.h shim (stb_image) that would otherwise shadow libc++'s <cstdlib>
    # which libc++ headers pull in via <new>/std::nothrow. All repo headers are
    # included with quotes, so -iquote is sufficient.
    #
    # libs/gui/color/include is always added: every SDK app links colour math
    # (see the old FM_SYS_INC / ST_SYS_INC / TM_SYS_INC / LIBDESKTOP_SYS_INC,
    # each of which lists it).
    target_compile_options(${name}-objects PRIVATE
        -iquote ${KYUZEN_ROOT}/include
    )
    target_include_directories(${name}-objects PRIVATE
        "${KYUZEN_ROOT}/libs/gui/color/include"
    )
    # Nothing may compile before the SDK is staged.
    add_dependencies(${name}-objects kyuzen-sdk-cpp)

    if(ARG_INCLUDES)
        target_include_directories(${name}-objects PRIVATE ${ARG_INCLUDES})
    endif()
    if(ARG_DEFINES)
        target_compile_definitions(${name}-objects PRIVATE ${ARG_DEFINES})
    endif()
    if(ARG_COMPILE_OPTIONS)
        target_compile_options(${name}-objects PRIVATE ${ARG_COMPILE_OPTIONS})
    endif()

    kyuzen_placeholder_object(_placeholder)
    add_executable(${name} "${_placeholder}")
    set_source_files_properties("${_placeholder}" PROPERTIES
        EXTERNAL_OBJECT TRUE GENERATED TRUE)
    set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)

    # -----------------------------------------------------------------------
    # LINK ORDER — via a generated response file.
    #
    # The old link line interleaves archives between object groups:
    #     settings objects, userlib.o, ..., kzraster_ft.o, libfreetype.a,
    #     color_utils.o, widget abi, widget layers
    # and archive position is load-bearing: a .a only resolves symbols
    # referenced by things ALREADY on the command line.
    #
    # CMake's LINK_LIBRARIES emits every object before every archive, so the
    # interleaving is lost and the FT archive ends up after libc.a — too late
    # to resolve the kzraster_ft.o references.
    #
    # LINK_FLAGS preserves the order but the command line then exceeds the
    # Windows 8191-character limit. So the ordered list is written to a
    # response file at configure time and handed to the linker as @file; CMake
    # substitutes the generator expressions when it writes the build rules.
    # -----------------------------------------------------------------------
    set(_link_items "")
    foreach(_lib IN LISTS ARG_LIBS)
        if(TARGET ${_lib})
            get_target_property(_type ${_lib} TYPE)
            if(_type STREQUAL "OBJECT_LIBRARY")
                list(APPEND _link_items "$<TARGET_OBJECTS:${_lib}>")
            elseif(_type STREQUAL "INTERFACE_LIBRARY")
                get_target_property(_deps ${_lib} INTERFACE_LINK_LIBRARIES)
                if(_deps)
                    kyuzen_expand_objects(_sub ${_deps})
                    list(APPEND _link_items ${_sub})
                endif()
            elseif(_type STREQUAL "STATIC_LIBRARY")
                list(APPEND _link_items "$<TARGET_FILE:${_lib}>")
                add_dependencies(${name} ${_lib})
            else()
                list(APPEND _link_items "${_lib}")
            endif()
        else()
            list(APPEND _link_items "${_lib}")
        endif()
    endforeach()

    # Full ordered link line, in the exact sequence the old recipe used.
    set(_full_link_sequence
        $<TARGET_OBJECTS:${name}-objects>
        ${_link_items}
        "${KYUZEN_LIBCXXRT_ARCHIVE}"
        "${KYUZEN_SDK_CPP_CXXRT}"
        "${KYUZEN_SDK_C_CRT}"
        "${KYUZEN_SDK_C_LIB}"
    )

    # One item per line, so the file needs no shell quoting rules.
    set(_rsp_file "${CMAKE_BINARY_DIR}/obj/${name}.link.rsp")
    file(GENERATE
        OUTPUT "${_rsp_file}"
        CONTENT "$<JOIN:${_full_link_sequence},\n>\n"
    )

    target_link_options(${name} PRIVATE
        -m elf_x86_64
        -nostdlib
        -T ${KYUZEN_SDK_CPP_LD}
        "@${_rsp_file}"
    )

    # Build-ordering only: the objects are emitted from the response file.
    add_dependencies(${name} ${name}-objects)

    # The SDK artifacts must exist before the link.
    add_dependencies(${name} kyuzen-sdk-cpp)

    kyuzen_output_basename(${name} _out_name)
    set_target_properties(${name} PROPERTIES
        OUTPUT_NAME "${_out_name}"
        SUFFIX ".elf"
        RUNTIME_OUTPUT_DIRECTORY "${KYUZEN_ELF_DIR}"
        LINK_DEPENDS "${KYUZEN_SDK_CPP_LD}"
    )
endfunction()

# ---------------------------------------------------------------------------
# kyuzen_verify_user_elf(<target>)
#
# Post-build checks that the old Makefile ran on every SDK-built ELF. These
# catch real regressions that would otherwise only appear at boot:
#   - entry point must be _start
#   - no unresolved symbols (a missing archive member)
#   - no exception/thread runtime pulled in (there is none on this target)
#   - no SSE / x87 instructions (the kernel never enables CR4.OSFXSR, so any
#     such instruction faults at runtime)
#
# Implemented as a CMake script so it works without grep/sed/bash.
# ---------------------------------------------------------------------------
function(kyuzen_verify_user_elf target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DELF=$<TARGET_FILE:${target}>
                -DKYUZEN_NM=${KYUZEN_LLVM_NM}
                -DKYUZEN_OBJDUMP=${KYUZEN_LLVM_OBJDUMP}
                -DEXPECTED_ENTRY=_start
                -P ${KYUZEN_ROOT}/cmake/verify_user_elf.cmake
        COMMENT "Verifying ${target} (entry, undefined symbols, SSE/x87)"
        VERBATIM
    )
endfunction()
