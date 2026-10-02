# ============================================================================
# libs/cpp/CMakeLists.txt — the Kyuzen C++ SDK stage.
#
# Sits on top of the C SDK. Adds:
#   - the C++ runtime (operator new/delete, __cxa_guard_*, __dso_handle,
#     __cxa_pure_virtual, __cxa_atexit/finalize) -> cxxrt.o
#   - a libc++ subset archive -> libcxxrt.a
#   - the libc++ HEADER CLOSURE for 8 public headers -> include/
#   - the public linker script + the kyuzen-c++ wrapper
#
# The header closure is COMPUTED at build time via `clang++ -M` for each public
# header (docs/development/build-system-audit.md §8.2), not maintained as a manual list — so it
# follows the pinned LLVM tree automatically.
#
# WHY ONLY 2 libc++ SOURCES: string.cpp and algorithm.cpp cannot compile under
# the -mno-sse ABI (they instantiate float/double paths). Their non-FP slices
# are supplied by Kyuzen-authored TUs that take the upstream code verbatim but
# instantiate only the integer/char variants.
# ============================================================================

set(_cpp_dir  "${KYUZEN_ROOT}/libs/cpp")
set(_libcxx   "${KYUZEN_ROOT}/third_party/stdlib/llvm-project/libcxx")
set(_libcxx_inc "${_libcxx}/include")

set(_sdk_cpp_dir    "${KYUZEN_SDK_DIR}/cpp")
set(_sdk_cpp_inc    "${_sdk_cpp_dir}/include")
set(_sdk_cpp_cxxrt  "${_sdk_cpp_dir}/cxxrt.o")
set(_sdk_cpp_ld     "${_sdk_cpp_dir}/linker/app.ld")
set(_sdk_cpp_wrapper "${_sdk_cpp_dir}/bin/kyuzen-c++")
set(_sdk_cpp_stage  "${_sdk_cpp_dir}/.staged")
set(_libcxxrt_archive "${_sdk_cpp_dir}/lib/libcxxrt.a")

# ---------------------------------------------------------------------------
# Public libc++ headers whose closure gets staged. `functional` is staged
# because <algorithm>'s __sort uses ranges::less, NOT because it is part of the
# supported API surface (the smoke app is denied it by a guard).
# ---------------------------------------------------------------------------
set(_libcxx_public_headers
    array algorithm memory string string_view type_traits utility vector new functional
)

set(_sdk_cpp_site_files
    "${_cpp_dir}/include/__config_site"
    "${_cpp_dir}/include/__assertion_handler"
)

set(_sdk_cpp_public_headers
    "${_cpp_dir}/include/kyuzen/config.hpp"
    "${_cpp_dir}/include/kyuzen/app.hpp"
    "${_cpp_dir}/include/kyuzen/panic.hpp"
)

file(GLOB _libdesktop_public_headers
    "${KYUZEN_ROOT}/libs/gui/libdesktop/include/kyuzen/desktop/*.hpp")

# ===========================================================================
# C++ runtime -> cxxrt.o
#
# This is NOT from LLVM: it is the Kyuzen-authored runtime. It must define
# __cxa_atexit itself because libc.a's version only finalises dso==NULL while
# Clang registers destructors with dso=&__dso_handle — using libc's version
# means global destructors never run.
# ===========================================================================
set(_cxxrt_src "${KYUZEN_ROOT}/libs/c/libc-port/src/kyuzen_cxx_runtime.cpp")

add_custom_command(
    OUTPUT "${_sdk_cpp_cxxrt}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_sdk_cpp_dir}"
    COMMAND ${KYUZEN_CLANGXX}
            --target=x86_64-pc-none-elf
            -ffreestanding -nostdlib -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float
            -std=c++17 -fno-exceptions -fno-rtti -nostdinc++ -O2 -fno-builtin
            -D_LIBCPP_BUILDING_LIBRARY
            -D_LIBCPP_AVAILABILITY_MINIMUM_HEADER_VERSION=2
            # Relative paths, as in the old recipe — see the note in the
            # libcxxrt loop below.
            -isystem libs/cpp/include
            -isystem third_party/stdlib/llvm-project/libcxx/include
            -isystem build/sdk/c/include
            -c libs/c/libc-port/src/kyuzen_cxx_runtime.cpp
            -o "${_sdk_cpp_cxxrt}"
    DEPENDS
        "${_cxxrt_src}"
        ${_sdk_cpp_site_files}
        "${KYUZEN_SDK_C_STAGE}"
    WORKING_DIRECTORY "${KYUZEN_ROOT}"
    COMMENT "Compiling C++ runtime (cxxrt.o)"
    VERBATIM
)

# Three parallel lists: source, archive member name, C++ standard.
#
# The member names are pinned to the short forms the old build produced
# (shims.o, string_inst.o, sort_inst.o) because the archive's member list is
# part of the artifact and must stay byte-identical.
#
# Standards: the upstream TUs are compiled at the archive standard (C++20);
# the string/sort slices are compiled at the app standard (C++17) so their
# instantiations match the extern-template declarations on the app side.
set(_libcxxrt_src_list
    "${_libcxx}/src/stdexcept.cpp"
    "${_libcxx}/src/verbose_abort.cpp"
    "${KYUZEN_ROOT}/libs/c/libc-port/src/kyuzen_libcxx_shims.cpp"
    "${KYUZEN_ROOT}/libs/c/libc-port/src/kyuzen_libcxx_string_inst.cpp"
    "${KYUZEN_ROOT}/libs/c/libc-port/src/kyuzen_libcxx_sort_inst.cpp"
)
set(_libcxxrt_name_list
    stdexcept
    verbose_abort
    shims
    string_inst
    sort_inst
)
set(_libcxxrt_std_list
    c++20
    c++20
    c++20
    c++17
    c++20
)

set(_libcxxrt_prologue "${KYUZEN_ROOT}/libs/c/libc-port/src/kyuzen_libcxx_prologue.h")

set(_libcxxrt_objs "")
list(LENGTH _libcxxrt_src_list _libcxxrt_count)

foreach(_i RANGE 0 ${_libcxxrt_count})
    if(_i GREATER_EQUAL _libcxxrt_count)
        break()
    endif()
    list(GET _libcxxrt_src_list  ${_i} _src)
    list(GET _libcxxrt_name_list ${_i} _name)
    list(GET _libcxxrt_std_list  ${_i} _std_ver)

    set(_obj "${KYUZEN_LIBC_DIR}/libcxxrt/${_name}.o")
    list(APPEND _libcxxrt_objs "${_obj}")

    add_custom_command(
        OUTPUT "${_obj}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${KYUZEN_LIBC_DIR}/libcxxrt"
        COMMAND ${KYUZEN_CLANGXX}
                --target=x86_64-pc-none-elf
                -ffreestanding -nostdlib -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float
                -std=${_std_ver} -fno-exceptions -fno-rtti -nostdinc++ -O2 -fno-builtin
                -D_LIBCPP_BUILDING_LIBRARY
                -D_LIBCPP_AVAILABILITY_MINIMUM_HEADER_VERSION=2
                # Relative include paths: LLVM's headers use __FILE__-derived
                # assertions, so absolute paths change the emitted object.
                # Also, `-include <abs-windows-path>` is rejected outright
                # (clang reads `E:` as a drive prefix in a header name).
                -isystem libs/cpp/include
                -isystem third_party/stdlib/llvm-project/libcxx/include
                -isystem build/sdk/c/include
                -include libs/c/libc-port/src/kyuzen_libcxx_prologue.h
                -c ${_src}
                -o "${_obj}"
        DEPENDS
            "${_src}"
            ${_sdk_cpp_site_files}
            "${_libcxxrt_prologue}"
            "${KYUZEN_SDK_C_STAGE}"
        WORKING_DIRECTORY "${KYUZEN_ROOT}"
        COMMENT "Compiling libc++ runtime: ${_name}.cpp"
        VERBATIM
    )
endforeach()

add_custom_command(
    OUTPUT "${_libcxxrt_archive}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_sdk_cpp_dir}/lib"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${_libcxxrt_archive}"
    COMMAND ${KYUZEN_LLVM_AR} rcs "${_libcxxrt_archive}" ${_libcxxrt_objs}
    DEPENDS ${_libcxxrt_objs}
    COMMENT "Archiving libcxxrt.a"
    VERBATIM
)

# ===========================================================================
# Stage the C++ SDK.
# ===========================================================================
# The staged files whose ONLY producer is this command. cxxrt.o and
# libcxxrt.a have their own custom commands above and are listed in DEPENDS
# instead — naming them here too would create a duplicate rule.
add_custom_command(
    OUTPUT
        "${_sdk_cpp_ld}"
        "${_sdk_cpp_wrapper}"
        "${_sdk_cpp_stage}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_sdk_cpp_inc}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_sdk_cpp_inc}/kyuzen/desktop"
            "${_sdk_cpp_dir}/bin"
            "${_sdk_cpp_dir}/linker"
    # Site config + public Kyuzen headers.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_sdk_cpp_site_files} "${_sdk_cpp_inc}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_sdk_cpp_public_headers} "${_sdk_cpp_inc}/kyuzen/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_libdesktop_public_headers} "${_sdk_cpp_inc}/kyuzen/desktop/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_cpp_dir}/linker/app.ld" "${_sdk_cpp_ld}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_cpp_dir}/bin/kyuzen-c++" "${_sdk_cpp_wrapper}"
    # Compute and copy the libc++ header closure.
    #
    # The flag list is passed as a single SEMICOLON-separated argument so the
    # script receives it intact. Individual -D... entries must NOT appear as
    # bare command-line arguments here: `cmake -P` would parse them as cache
    # definitions of its own.
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${_libcxx_inc}
            -DDST=${_sdk_cpp_inc}
            -DKYUZEN_CLANGXX=${KYUZEN_CLANGXX}
            "-DKYUZEN_CXXFLAGS=--target=x86_64-pc-none-elf;-ffreestanding;-nostdlib;-mno-red-zone;-mno-sse;-mno-sse2;-mno-mmx;-msoft-float;-std=c++17;-fno-exceptions;-fno-rtti;-O2;-fno-builtin;-D_LIBCPP_BUILDING_LIBRARY;-D_LIBCPP_AVAILABILITY_MINIMUM_HEADER_VERSION=2"
            -DLIBCXX_INC=${_libcxx_inc}
            -DWORK_DIR=${CMAKE_BINARY_DIR}/libcxx-header-probe
            -DSITE_INC=${_cpp_dir}/include
            -DCSDK_INC=${KYUZEN_SDK_C_INC}
            "-DHEADERS=${_libcxx_public_headers}"
            -P ${KYUZEN_ROOT}/cmake/stage_libcxx_headers.cmake
    # Sanity guards.
    COMMAND ${CMAKE_COMMAND}
            -DCXXRT=${_sdk_cpp_cxxrt}
            -DARCHIVE=${_libcxxrt_archive}
            -DKYUZEN_NM=${KYUZEN_LLVM_NM}
            -DSDK_INC=${_sdk_cpp_inc}
            -DWRAPPER=${_sdk_cpp_wrapper}
            -P ${KYUZEN_ROOT}/cmake/verify_cpp_sdk.cmake
    COMMAND ${CMAKE_COMMAND} -E touch "${_sdk_cpp_stage}"
    DEPENDS
        "${_sdk_cpp_cxxrt}"
        "${_libcxxrt_archive}"
        "${_cpp_dir}/linker/app.ld"
        "${_cpp_dir}/bin/kyuzen-c++"
        ${_sdk_cpp_site_files}
        ${_sdk_cpp_public_headers}
        ${_libdesktop_public_headers}
        "${KYUZEN_SDK_C_STAGE}"
    COMMENT "Staging Kyuzen C++ SDK -> build/sdk/cpp"
    VERBATIM
)

add_custom_target(kyuzen-sdk-cpp DEPENDS "${_sdk_cpp_stage}")

set_source_files_properties("${_sdk_cpp_cxxrt}" "${_libcxxrt_archive}"
    PROPERTIES GENERATED TRUE)

# ---------------------------------------------------------------------------
# Interface target for C++ SDK consumers.
# ---------------------------------------------------------------------------
add_library(kyuzen-sdk-cpp-interface INTERFACE)
add_dependencies(kyuzen-sdk-cpp-interface kyuzen-sdk-cpp)
target_include_directories(kyuzen-sdk-cpp-interface INTERFACE
    "$<BUILD_INTERFACE:${_sdk_cpp_inc}>"
    "$<BUILD_INTERFACE:${KYUZEN_SDK_C_INC}>"
)

set(KYUZEN_SDK_CPP_INC     "${_sdk_cpp_inc}"     PARENT_SCOPE)
set(KYUZEN_SDK_CPP_CXXRT   "${_sdk_cpp_cxxrt}"   PARENT_SCOPE)
set(KYUZEN_SDK_CPP_LD      "${_sdk_cpp_ld}"      PARENT_SCOPE)
set(KYUZEN_SDK_CPP_WRAPPER "${_sdk_cpp_wrapper}" PARENT_SCOPE)
set(KYUZEN_SDK_CPP_STAGE   "${_sdk_cpp_stage}"   PARENT_SCOPE)
set(KYUZEN_LIBCXXRT_ARCHIVE "${_libcxxrt_archive}" PARENT_SCOPE)
