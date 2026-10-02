# ============================================================================
# KyuzenCompilerWrapper.cmake — depfile path normalisation, shared.
#
# WHY THIS IS A SEPARATE MODULE
#   Both build trees need it and neither may include the other's toolchain:
#
#     target build  cmake/KyuzenToolchain.cmake   (bare-metal triple)
#     host tests    tests/host/CMakeLists.txt     (host triple)
#
#   The wrapper itself is triple-agnostic — it runs the real compiler with
#   whatever arguments it is given and only post-processes the depfile — so it
#   is factored out here instead of being duplicated.
#
#   It lives outside KyuzenToolchain.cmake because tests/host must NOT include
#   that file: it sets --target=x86_64-pc-none-elf and
#   CMAKE_TRY_COMPILE_TARGET_TYPE, which would make the host tests target the
#   bare-metal ABI and fail to link against the host libc.
#
# THE PROBLEM
#   MSYS2 ships two clang builds and they report dependencies differently:
#
#     usr/bin (MSYS/cygwin) -> /usr/lib/clang/21/include/stdint.h   (POSIX)
#     clang64/bin (MINGW)   -> E:/Tools/msys2/clang64/include/...   (native)
#
#   Ninja is a NATIVE Windows binary: it cannot resolve /usr/..., so it treats
#   every such dependency as MISSING and the affected target is permanently out
#   of date. The symptom is badly misleading — a no-op `ninja` reports hundreds
#   of rebuilds, and `ninja -d explain` blames "stdint.h is dirty" although the
#   file is untouched and older than the object it supposedly invalidates.
#
#   The MINGW clang emits native paths but is LLVM 22 while the parity baseline
#   was produced with LLVM 21 — and it generates a byte-different kernel. The
#   whole point of this migration is that the build system changes while the
#   output does not, so the MSYS clang stays and the depfile is fixed instead.
#
# WHAT IT DOES
#   Generates build-tools/kyuzen-cc and build-tools/kyuzen-cxx from
#   cmake/kyuzen-cc.in (with @MSYS_ROOT@ substituted), then hands back the
#   command lines to use as CMAKE_<LANG>_COMPILER.
# ============================================================================

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# kyuzen_install_compiler_wrapper(<clang> <clangxx>)
#
# Generates the wrappers next to the source tree and sets, in the CALLER's
# scope:
#
#   KYUZEN_WRAPPED_CC    command line to use as CMAKE_C_COMPILER
#   KYUZEN_WRAPPED_CXX   command line to use as CMAKE_CXX_COMPILER
#
# If the template or `sh` cannot be found the wrapped variables are set to the
# bare compiler paths, so the build still works — just with the permanent-dirty
# behaviour described above, which is far better than failing outright.
# ---------------------------------------------------------------------------
function(kyuzen_install_compiler_wrapper clang clangxx)
    set(_bare_cc  "${clang}")
    set(_bare_cxx "${clangxx}")

    find_program(_sh NAMES sh)

    # Derive the MSYS root from the compiler's own location: <root>/usr/bin/clang.
    # The wrapper needs it to turn /usr/... into an absolute Windows path.
    get_filename_component(_clang_bin_dir "${clang}" DIRECTORY)
    get_filename_component(_clang_usr_dir "${_clang_bin_dir}" DIRECTORY)
    get_filename_component(_msys_root     "${_clang_usr_dir}" DIRECTORY)

    set(_wrapper_in "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/kyuzen-cc.in")
    set(_wrapper_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../build-tools")
    set(_wrapper_cc  "${_wrapper_dir}/kyuzen-cc")
    set(_wrapper_cxx "${_wrapper_dir}/kyuzen-cxx")

    if(_sh AND EXISTS "${_wrapper_in}")
        file(MAKE_DIRECTORY "${_wrapper_dir}")
        file(READ "${_wrapper_in}" _src)
        string(REPLACE "@MSYS_ROOT@" "${_msys_root}" _src "${_src}")
        file(WRITE "${_wrapper_cc}"  "${_src}")
        file(WRITE "${_wrapper_cxx}" "${_src}")

        set(KYUZEN_WRAPPED_CC  "${_sh};${_wrapper_cc};${clang}"    PARENT_SCOPE)
        set(KYUZEN_WRAPPED_CXX "${_sh};${_wrapper_cxx};${clangxx}" PARENT_SCOPE)
        return()
    endif()

    if(NOT _sh)
        message(WARNING
            "kyuzen: `sh` not found, so compiler depfiles cannot be normalised. "
            "Ninja will treat POSIX dependency paths as missing and rebuild "
            "everything on every invocation. Put MSYS2's usr/bin on PATH.")
    endif()

    set(KYUZEN_WRAPPED_CC  "${_bare_cc}"  PARENT_SCOPE)
    set(KYUZEN_WRAPPED_CXX "${_bare_cxx}" PARENT_SCOPE)
endfunction()
