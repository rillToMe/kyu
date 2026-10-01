# ============================================================================
# stage_libcxx_headers.cmake — compute and copy the libc++ header closure.
#
# For each requested public header, ask the compiler which files it actually
# includes (`clang++ -M`), keep only those under the libc++ include root, and
# copy them into the SDK at their relative paths.
#
# This is computed rather than listed so that it follows the pinned LLVM tree
# automatically. The old Makefile did the same thing with a shell pipeline
# (Makefile lines ~1231-1237); a failed dependency computation must fail the
# build, not silently produce an incomplete SDK.
#
#   cmake -DSRC=<libcxx/include> -DDST=<sdk include dir>
#         -DKYUZEN_CLANGXX=<clang++> -DKYUZEN_CXXFLAGS=<;list;of;flags>
#         -DSITE_INC=<kyuzen site include> -DCSDK_INC=<C SDK include>
#         -DHEADERS=<;public;headers>
#         -P stage_libcxx_headers.cmake
# ============================================================================

if(NOT SRC OR NOT DST OR NOT KYUZEN_CLANGXX)
    message(FATAL_ERROR "stage_libcxx_headers: SRC, DST and KYUZEN_CLANGXX are required")
endif()
if(NOT IS_DIRECTORY "${SRC}")
    message(FATAL_ERROR "stage_libcxx_headers: libc++ include dir not found: ${SRC}")
endif()

# Scratch space for the probe TUs. Must NOT be inside DST — see below.
if(NOT WORK_DIR)
    set(WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")

# Build the common compile-flag list once.
#
# LIBCXX_INC is added explicitly: the probe TUs `#include <array>` etc., and
# the SDK flag set does not necessarily put the libc++ include root on the
# search path (it may carry -nostdinc++ to stay hermetic from the host).
set(_flags ${KYUZEN_CXXFLAGS})
if(LIBCXX_INC)
    list(APPEND _flags -isystem "${LIBCXX_INC}")
endif()
if(SITE_INC)
    list(APPEND _flags -isystem "${SITE_INC}")
endif()
if(CSDK_INC)
    list(APPEND _flags -isystem "${CSDK_INC}")
endif()

set(_copied 0)
# The destination root must exist before any file is copied into it: the
# staging recipe removes the directory first, and copy_file does not create
# parent directories.
file(MAKE_DIRECTORY "${DST}")

# Probe TUs live OUTSIDE the destination tree. If they were placed under DST,
# the dependency scan would find them as part of the closure and try to copy
# each probe into itself.
set(_probe_dir "${WORK_DIR}/libcxx-header-probe")
file(MAKE_DIRECTORY "${_probe_dir}")

foreach(_hdr IN LISTS HEADERS)
    # Write a one-line probe TU that includes just this header, then ask the
    # compiler for its include closure.
    set(_probe "${_probe_dir}/${_hdr}.cpp")
    file(WRITE "${_probe}" "#include <${_hdr}>\n")

    execute_process(
        COMMAND ${KYUZEN_CLANGXX} ${_flags} -x c++ -M "${_probe}"
        OUTPUT_VARIABLE _deps
        ERROR_VARIABLE  _err
        RESULT_VARIABLE _rc
    )

    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "stage_libcxx_headers: dependency scan failed for <${_hdr}>:\n${_err}")
    endif()

    # Split the make-style output into file paths.
    #
    # `clang -M` emits "target: dep1 dep2 ...\n", so the LAST dependency keeps
    # the trailing newline. A path with a newline in it makes file(COPY_FILE)
    # fail with a bare "Invalid argument" — and only for whichever file
    # happens to sort last, which is why this looked arbitrary.
    string(REGEX REPLACE "[ \t]*\\\\\r?\n[ \t]*" " " _deps "${_deps}")
    string(REPLACE "\r" "" _deps "${_deps}")
    string(REPLACE "\n" "" _deps "${_deps}")
    string(STRIP "${_deps}" _deps)
    string(REPLACE " " ";" _dep_list "${_deps}")

    foreach(_dep IN LISTS _dep_list)
        # Keep only files inside the libc++ include root.
        string(FIND "${_dep}" "${SRC}/" _pos)
        if(_pos EQUAL 0)
            file(RELATIVE_PATH _rel "${SRC}" "${_dep}")
            get_filename_component(_rel_dir "${_rel}" DIRECTORY)
            if(_rel_dir)
                file(MAKE_DIRECTORY "${DST}/${_rel_dir}")
            endif()

            # Compare-then-copy rather than COPY_FILE ... ONLY_IF_DIFFERENT:
            # that option fails with "Invalid argument" when the destination
            # already holds an identical file (e.g. re-running the stage over a
            # previous partial result). Explicit hashing is predictable and
            # keeps the stage idempotent without touching timestamps.
            set(_dst_file "${DST}/${_rel}")
            set(_needs_copy TRUE)
            if(EXISTS "${_dst_file}")
                file(SHA256 "${_dep}" _dep_hash)
                file(SHA256 "${_dst_file}" _dst_hash)
                if(_dep_hash STREQUAL _dst_hash)
                    set(_needs_copy FALSE)
                endif()
            endif()
            if(_needs_copy)
                file(COPY_FILE "${_dep}" "${_dst_file}")
            endif()

            math(EXPR _copied "${_copied} + 1")
        endif()
    endforeach()
endforeach()

if(_copied EQUAL 0)
    message(FATAL_ERROR
        "stage_libcxx_headers: no headers were copied — the dependency scan "
        "produced nothing under ${SRC}")
endif()

# Remove the probe TUs so they never reach the SDK consumer.
file(REMOVE_RECURSE "${_probe_dir}")

# ---------------------------------------------------------------------------
# Verify each requested public header is now present.
# ---------------------------------------------------------------------------
foreach(_hdr IN LISTS HEADERS)
    if(NOT EXISTS "${DST}/${_hdr}")
        message(FATAL_ERROR
            "stage_libcxx_headers: public header <${_hdr}> was not staged")
    endif()
endforeach()
