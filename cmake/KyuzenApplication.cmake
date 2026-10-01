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
# An empty object used as the sole "source" of executables whose real content
# arrives as $<TARGET_OBJECTS:...>. add_executable requires at least one
# source, and the placeholder must contribute NOTHING to the linked image — no
# symbol, no section, no .strtab entry — or binary parity is lost.
#
# A C translation unit is unsuitable (STT_FILE + .comment), and nasm always
# emits an STT_FILE for its input. An empty .s compiled by clang produces an
# object with no symbols and no sections.
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
            DEPENDS "${_src}"
            COMMENT "Creating empty placeholder object"
            VERBATIM
        )
        add_custom_target(kyuzen-placeholder DEPENDS "${_obj}")
    endif()

    set(${out_var} "${_obj}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kyuzen_link_items(<out_var> <target-or-path>...)
#
# Normalise a mixed list of OBJECT libraries, STATIC libraries, and plain file
# paths into link items:
#
#   OBJECT library  -> $<TARGET_OBJECTS:name>   (placed exactly where declared)
#   STATIC library  -> the target name          (archive, resolved in order)
#   file path       -> unchanged
#
# OBJECT libraries must use $<TARGET_OBJECTS:...>; passing the target name
# lets CMake reorder the objects, which changes section layout and breaks
# binary parity with the Make baseline.
# ---------------------------------------------------------------------------
function(kyuzen_link_items out_var)
    set(_items "")
    foreach(_lib IN LISTS ARGN)
        if(TARGET ${_lib})
            get_target_property(_type ${_lib} TYPE)
            if(_type STREQUAL "OBJECT_LIBRARY")
                list(APPEND _items "$<TARGET_OBJECTS:${_lib}>")
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
# kyuzen_add_plain_app(<name>
#     SOURCES   <src>...
#     [FLAGS    <flag-set-target>]   default: kyuzen-flags-app
#     [LIBS     <target-or-file>...]
#     [INCLUDES <dir>...]
#     [COMPILE_OPTIONS <opt>...]
#     [LINK_OPTIONS    <opt>...]
#     [DEFINES  <def>...]
# )
#
# Simple freestanding C app: ENTRY(main) via apps/app.ld, links userlib plus
# whatever extra libraries the app declares.
#
# FLAGS selects the canonical flag set. The old apps/Makefile used TWO sets for
# this group of targets:
#   CFLAGS_APP (-O0)  apps/*.c            — application code
#   CFLAGS_LIB (-O2)  system/*.c          — utilities treated as library code
# Passing the wrong one changes generated code and breaks binary parity.
#
# NOTE: passing `-O2` via COMPILE_OPTIONS does NOT work — CMake places a
# target's own options BEFORE those inherited from an INTERFACE library, and
# clang honours the LAST -O, so the interface's -O0 would win. The flag set
# itself must be the right one.
# ---------------------------------------------------------------------------
function(kyuzen_add_plain_app name)
    cmake_parse_arguments(ARG
        ""
        "FLAGS"
        "SOURCES;LIBS;INCLUDES;COMPILE_OPTIONS;LINK_OPTIONS;DEFINES"
        ${ARGN}
    )

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "kyuzen_add_plain_app(${name}): SOURCES is required")
    endif()

    if(NOT ARG_FLAGS)
        set(ARG_FLAGS kyuzen-flags-app)
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
        -T ${KYUZEN_ROOT}/apps/app.ld
    )

    kyuzen_output_basename(${name} _out_name)
    set_target_properties(${name} PROPERTIES
        OUTPUT_NAME "${_out_name}"
        SUFFIX ".elf"
        RUNTIME_OUTPUT_DIRECTORY "${KYUZEN_ELF_DIR}"
        LINK_DEPENDS "${KYUZEN_ROOT}/apps/app.ld"
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

    add_executable(${name} ${ARG_SOURCES})

    target_link_libraries(${name} PRIVATE kyuzen-flags-sdk-cpp)
    target_link_options(${name} PRIVATE
        -m elf_x86_64
        -nostdlib
        -T ${KYUZEN_SDK_DIR}/cpp/linker/app.ld
    )

    kyuzen_output_basename(${name} _out_name)
    set_target_properties(${name} PROPERTIES
        OUTPUT_NAME "${_out_name}"
        SUFFIX ".elf"
        RUNTIME_OUTPUT_DIRECTORY "${KYUZEN_ELF_DIR}"
        LINK_DEPENDS "${KYUZEN_SDK_DIR}/cpp/linker/app.ld"
    )

    if(ARG_INCLUDES)
        target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
    endif()
    if(ARG_DEFINES)
        target_compile_definitions(${name} PRIVATE ${ARG_DEFINES})
    endif()
    if(ARG_COMPILE_OPTIONS)
        target_compile_options(${name} PRIVATE ${ARG_COMPILE_OPTIONS})
    endif()
    if(ARG_LIBS)
        target_link_libraries(${name} PRIVATE ${ARG_LIBS})
    endif()
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
