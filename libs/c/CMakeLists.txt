# ============================================================================
# libs/c/CMakeLists.txt — LLVM libc (freestanding) + the Kyuzen C SDK stage.
#
# This is the most complex integration in the project (docs/development/build-system-audit.md
# §8.1). It is a NESTED build: LLVM's `runtimes` superbuild is configured and
# built as an external step with its own binary directory and its own
# generator, then its output is wrapped into a small public SDK.
#
# WHY NOT add_subdirectory(): the LLVM build needs its own CMake invocation
# with a different generator, a different toolchain cache, and a different
# binary dir. Making it a subdirectory would leak its targets, options, and
# toolchain into the KyuzenOS project.
#
# Pipeline (mirrors the old Makefile's libc-phase0..4):
#   1. configure  <llvm>/runtimes  -> build/libc/cmake
#   2. build      target `libc`    -> libc.a
#   3. copy       libc.a           -> build/libc/x86_64-pc-none-elf/lib/libc.a
#   4. compile    port layer       -> kyuzen_libc_port.o  (the CRT)
#   5. stage      build/sdk/c      -> public headers + libc.a + crt.o + app.ld
#
# Step 5 is guarded by llvm-nm greps: a stale libc.a that silently lacks
# printf/malloc/qsort/timespec_get is a real failure mode the old Makefile
# checked for explicitly (and the guards are kept).
# ============================================================================

set(_libc_src    "${KYUZEN_ROOT}/third_party/stdlib/llvm-project")
set(_libc_triple "x86_64-pc-none-elf")
set(_libc_out    "${KYUZEN_LIBC_DIR}")
set(_libc_cmake  "${_libc_out}/cmake")
set(_libc_inc    "${_libc_cmake}/libc/include")
set(_libc_archive "${_libc_out}/${_libc_triple}/lib/libc.a")
set(_libc_cache  "${_libc_src}/libc/cmake/caches/${_libc_triple}.cmake")
set(_libc_cfg    "${_libc_src}/libc/config/baremetal/x86_64")

set(_sdk_dir   "${KYUZEN_SDK_DIR}/c")
set(_sdk_inc   "${_sdk_dir}/include")
set(_sdk_lib   "${_sdk_dir}/lib/libc.a")
set(_sdk_crt   "${_sdk_dir}/crt/crt.o")
set(_sdk_ld    "${_sdk_dir}/linker/app.ld")
set(_sdk_ld_src "${KYUZEN_ROOT}/libs/c/linker/app.ld")
set(_sdk_stage "${_sdk_dir}/.staged")

if(NOT EXISTS "${_libc_src}/runtimes/CMakeLists.txt")
    message(FATAL_ERROR
        "LLVM libc sources are missing at ${_libc_src}.\n"
        "third_party/stdlib/llvm-project is a gitlink WITHOUT a .gitmodules "
        "entry (docs/development/build-system-audit.md §13.4), so a fresh clone cannot "
        "materialise it.\n"
        "Either populate that directory, or configure with "
        "-DKYUZEN_BUILD_LIBC_SDK=OFF.")
endif()

# ===========================================================================
# 1-3. LLVM libc archive.
#
# Configure and build are two separate custom commands so that a failed build
# does not force a reconfigure on the next run.
# ===========================================================================
add_custom_command(
    OUTPUT "${_libc_cmake}/CMakeCache.txt"
    COMMAND ${CMAKE_COMMAND}
            -S "${_libc_src}/runtimes"
            -B "${_libc_cmake}"
            -G "Unix Makefiles"
            -C "${_libc_cache}"
            -DCMAKE_C_COMPILER=${KYUZEN_CLANG}
            -DCMAKE_CXX_COMPILER=${KYUZEN_CLANGXX}
            -DCMAKE_BUILD_TYPE=Release
    DEPENDS
        "${_libc_cache}"
        "${_libc_cfg}/entrypoints.txt"
        "${_libc_cfg}/headers.txt"
    COMMENT "Configuring LLVM libc (nested CMake, Unix Makefiles)"
    VERBATIM
)

add_custom_command(
    OUTPUT "${_libc_archive}"
    COMMAND ${CMAKE_COMMAND} --build "${_libc_cmake}" --target libc -- -j8
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_libc_out}/${_libc_triple}/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_libc_cmake}/libc/lib/libc.a" "${_libc_archive}"
    DEPENDS "${_libc_cmake}/CMakeCache.txt"
    COMMENT "Building LLVM libc.a"
    VERBATIM
)

# ===========================================================================
# 4. Kyuzen port layer -> crt.o
#
# Compiled against libc's INTERNAL headers with libc's own namespace so it can
# replace the built-in freelist_heap. Its backing memory comes from syscall #9
# (uheap), not from linker symbols _end/__llvm_libc_heap_limit.
#
# WHY RELATIVE INCLUDE PATHS: the old recipe ran from the repo root and passed
# `-Ithird_party/stdlib/llvm-project/libc`. LLVM libc's headers use
# `__FILE__`-derived assertions, so an absolute -I produces a different object
# (+1664 bytes here). WORKING_DIRECTORY=root plus relative paths reproduces the
# original invocation exactly.
# ===========================================================================
set(_port_obj "${_libc_out}/port/kyuzen_libc_port.o")

add_custom_command(
    OUTPUT "${_port_obj}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_libc_out}/port"
    COMMAND ${KYUZEN_CLANGXX}
            --target=${_libc_triple}
            -ffreestanding -nostdlib -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float
            -std=gnu++17 -fno-exceptions -fno-rtti -O2 -fno-builtin
            -fno-unwind-tables -fno-asynchronous-unwind-tables
            -fvisibility-inlines-hidden
            -Ithird_party/stdlib/llvm-project/libc
            -Ibuild/libc/cmake/libc
            -isystem build/libc/cmake/libc/include
            -DLIBC_NAMESPACE=${KYUZEN_LIBC_NAMESPACE}
            -DLIBC_FULL_BUILD
            -DLIBC_TARGET_OS_IS_BAREMETAL
            -DLIBC_ERRNO_MODE=LIBC_ERRNO_MODE_EXTERNAL
            -DLIBC_THREAD_MODE=LIBC_THREAD_MODE_SINGLE
            -DLIBC_COPT_PUBLIC_PACKAGING
            -c libs/c/libc-port/src/kyuzen_libc_port.cpp
            -o "${_port_obj}"
    DEPENDS
        "${KYUZEN_ROOT}/libs/c/libc-port/src/kyuzen_libc_port.cpp"
        "${_libc_archive}"
    WORKING_DIRECTORY "${KYUZEN_ROOT}"
    COMMENT "Compiling Kyuzen libc port layer (CRT)"
    VERBATIM
)

# ===========================================================================
# 5. Stage the public C SDK.
#
# Only PUBLIC headers reach the SDK: the top level, llvm-libc-types/, and
# llvm-libc-macros/ (including the baremetal subdirectory that time.h needs).
# The generated include tree also contains CMakeFiles/ and Makefiles, which
# must NOT be copied — hence the explicit re-create of the include directory.
# ===========================================================================
add_custom_command(
    OUTPUT "${_sdk_lib}" "${_sdk_crt}" "${_sdk_ld}" "${_sdk_stage}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_sdk_inc}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_sdk_inc}"
            "${_sdk_dir}/lib"
            "${_sdk_dir}/crt"
            "${_sdk_dir}/linker"
    # Headers: ONLY *.h files, and only from the three public locations.
    # A directory copy would also drag in the build-system files that live in
    # the generated include tree (CMakeFiles/, Makefile, cmake_install.cmake) —
    # the old Makefile avoided this with explicit `cp <dir>/*.h` globs.
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${_libc_inc}
            -DDST=${_sdk_inc}
            -P ${KYUZEN_ROOT}/cmake/stage_libc_headers.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_libc_archive}" "${_sdk_lib}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_port_obj}"     "${_sdk_crt}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sdk_ld_src}"   "${_sdk_ld}"
    # Sanity guards: a stale archive missing these symbols is a real failure
    # mode, not a hypothetical one.
    COMMAND ${CMAKE_COMMAND}
            -DSDK_LIB=${_sdk_lib}
            -DSDK_INC=${_sdk_inc}
            -DKYUZEN_NM=${KYUZEN_LLVM_NM}
            -P ${KYUZEN_ROOT}/cmake/verify_c_sdk.cmake
    COMMAND ${CMAKE_COMMAND} -E touch "${_sdk_stage}"
    DEPENDS
        "${_libc_archive}"
        "${_port_obj}"
        "${_sdk_ld_src}"
    COMMENT "Staging Kyuzen C SDK -> build/sdk/c"
    VERBATIM
)

add_custom_target(kyuzen-sdk-c DEPENDS "${_sdk_stage}")

# ---------------------------------------------------------------------------
# Declare the staged files as GENERATED so nothing treats them as sources that
# should exist in the source tree.
# ---------------------------------------------------------------------------
set_source_files_properties("${_sdk_lib}" "${_sdk_crt}" "${_sdk_ld}"
    PROPERTIES GENERATED TRUE)

# ---------------------------------------------------------------------------
# Interface target: what an app must have in order to compile and link against
# the C SDK. Consumers use this instead of naming paths.
# ---------------------------------------------------------------------------
add_library(kyuzen-sdk-c-interface INTERFACE)
add_dependencies(kyuzen-sdk-c-interface kyuzen-sdk-c)
target_include_directories(kyuzen-sdk-c-interface INTERFACE
    "$<BUILD_INTERFACE:${_sdk_inc}>"
)
target_link_libraries(kyuzen-sdk-c-interface INTERFACE
    "$<BUILD_INTERFACE:${_sdk_crt}>"
    "$<BUILD_INTERFACE:${_sdk_lib}>"
)

# Export the paths for consumers that need the raw names (e.g. the C++ SDK).
set(KYUZEN_SDK_C_INC   "${_sdk_inc}"   PARENT_SCOPE)
set(KYUZEN_SDK_C_LIB   "${_sdk_lib}"   PARENT_SCOPE)
set(KYUZEN_SDK_C_CRT   "${_sdk_crt}"   PARENT_SCOPE)
set(KYUZEN_SDK_C_LD    "${_sdk_ld}"    PARENT_SCOPE)
set(KYUZEN_SDK_C_STAGE "${_sdk_stage}" PARENT_SCOPE)
set(KYUZEN_LIBC_ARCHIVE "${_libc_archive}" PARENT_SCOPE)
set(KYUZEN_LIBC_CMAKE_DIR "${_libc_cmake}" PARENT_SCOPE)
set(KYUZEN_LIBC_INCLUDE   "${_libc_inc}"   PARENT_SCOPE)
