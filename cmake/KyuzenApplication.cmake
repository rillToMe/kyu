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
# kyuzen_add_plain_app(<name>
#     SOURCES   <src>...
#     [LIBS     <target-or-file>...]
#     [INCLUDES <dir>...]
#     [COMPILE_OPTIONS <opt>...]
#     [LINK_OPTIONS    <opt>...]
#     [DEFINES  <def>...]
# )
#
# Simple freestanding C app: ENTRY(main) via apps/app.ld, links userlib plus
# whatever extra libraries the app declares. Used for system utilities
# (echo/cat/zen/init/login/shell), test ELFs, and the GUI apps that need the
# toolkit.
#
# Optimization level follows the old apps/Makefile split: app sources -O0,
# library sources -O2. Callers that need -O2 for the app source pass it via
# COMPILE_OPTIONS.
# ---------------------------------------------------------------------------
function(kyuzen_add_plain_app name)
    cmake_parse_arguments(ARG
        ""
        ""
        "SOURCES;LIBS;INCLUDES;COMPILE_OPTIONS;LINK_OPTIONS;DEFINES"
        ${ARGN}
    )

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "kyuzen_add_plain_app(${name}): SOURCES is required")
    endif()

    add_executable(${name} ${ARG_SOURCES})

    target_link_libraries(${name} PRIVATE kyuzen-flags-app)
    target_link_options(${name} PRIVATE
        -m elf_x86_64
        -nostdlib
        -T ${KYUZEN_ROOT}/apps/app.ld
    )

    # Every plain app is a user-space ELF that lands in build/apps/.
    set_target_properties(${name} PROPERTIES
        OUTPUT_NAME "${name}"
        SUFFIX ".elf"
        RUNTIME_OUTPUT_DIRECTORY "${KYUZEN_ELF_DIR}"
        LINK_DEPENDS "${KYUZEN_ROOT}/apps/app.ld"
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
    if(ARG_LINK_OPTIONS)
        target_link_options(${name} PRIVATE ${ARG_LINK_OPTIONS})
    endif()
    if(ARG_LIBS)
        target_link_libraries(${name} PRIVATE ${ARG_LIBS})
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

    set_target_properties(${name} PROPERTIES
        OUTPUT_NAME "${name}"
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
