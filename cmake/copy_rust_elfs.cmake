# ============================================================================
# copy_rust_elfs.cmake — copy cargo's output into build/apps/.
#
#   cmake -DSRC_DIR=<cargo release dir> -DDST_DIR=<elf dir>
#         "-DNAMES=a;b" -P copy_rust_elfs.cmake
#
# The copy is CONTENT-COMPARED rather than unconditional: an unconditional copy
# updates the mtime on every build, which makes the ISO look stale and forces a
# rebuild each time. The old Makefile used `cmp -s ... || cp ...` for the same
# reason.
# ============================================================================

if(NOT SRC_DIR OR NOT DST_DIR OR NOT NAMES)
    message(FATAL_ERROR "copy_rust_elfs: SRC_DIR, DST_DIR and NAMES are required")
endif()

if(NOT IS_DIRECTORY "${SRC_DIR}")
    message(FATAL_ERROR
        "copy_rust_elfs: cargo output directory not found: ${SRC_DIR}\n"
        "The cargo build did not produce the expected release artifacts.")
endif()

file(MAKE_DIRECTORY "${DST_DIR}")

foreach(_name IN LISTS NAMES)
    # Cargo emits the binary without a suffix on this target.
    set(_src "${SRC_DIR}/${_name}")
    if(NOT EXISTS "${_src}")
        # Fall back to the platform's executable suffix if present.
        if(EXISTS "${_src}.exe")
            set(_src "${_src}.exe")
        else()
            message(FATAL_ERROR
                "copy_rust_elfs: cargo did not produce '${_name}' in ${SRC_DIR}")
        endif()
    endif()

    set(_dst "${DST_DIR}/${_name}.elf")

    set(_needs_copy TRUE)
    if(EXISTS "${_dst}")
        file(SHA256 "${_src}" _src_hash)
        file(SHA256 "${_dst}" _dst_hash)
        if(_src_hash STREQUAL _dst_hash)
            set(_needs_copy FALSE)
        endif()
    endif()

    if(_needs_copy)
        file(COPY_FILE "${_src}" "${_dst}")
        message(STATUS "rust: copied ${_name}.elf")
    endif()
endforeach()
