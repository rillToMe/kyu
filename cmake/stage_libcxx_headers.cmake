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

# Build the common compile-flag list once.
set(_flags ${KYUZEN_CXXFLAGS})
if(SITE_INC)
    list(APPEND _flags -isystem "${SITE_INC}")
endif()
if(CSDK_INC)
    list(APPEND _flags -isystem "${CSDK_INC}")
endif()

set(_copied 0)
set(_probe_dir "${DST}/.kyuzen-probe")
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
    string(REGEX REPLACE "[ \t]*\\\\\r?\n[ \t]*" " " _deps "${_deps}")
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
            file(COPY_FILE "${_dep}" "${DST}/${_rel}" ONLY_IF_DIFFERENT)
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
