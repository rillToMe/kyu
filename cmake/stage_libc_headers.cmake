# ============================================================================
# stage_libc_headers.cmake — copy ONLY public libc headers into the C SDK.
#
# The generated LLVM libc include directory also contains build-system files
# (CMakeFiles/, Makefile, cmake_install.cmake) which must not reach the SDK.
# The old Makefile handled this with explicit `cp <dir>/*.h` globs; this script
# does the same but without a shell, and re-creates the target directory so the
# stage stays idempotent.
#
#   cmake -DSRC=<libc include dir> -DDST=<sdk include dir> -P stage_libc_headers.cmake
#
# Layout produced (matches the old $(SDK_INC)):
#   <DST>/*.h                                top-level public headers
#   <DST>/llvm-libc-types/*.h                type headers
#   <DST>/llvm-libc-macros/*.h               macro headers
#   <DST>/llvm-libc-macros/baremetal/*.h     per-OS macro headers
#                                            (time.h includes these under __ELF__)
# ============================================================================

if(NOT SRC OR NOT DST)
    message(FATAL_ERROR "stage_libc_headers: both -DSRC and -DDST are required")
endif()

if(NOT IS_DIRECTORY "${SRC}")
    message(FATAL_ERROR "stage_libc_headers: source directory not found: ${SRC}")
endif()

file(MAKE_DIRECTORY "${DST}")

# --- top-level public headers ----------------------------------------------
file(GLOB _top_headers RELATIVE "${SRC}" "${SRC}/*.h")
list(LENGTH _top_headers _top_count)
if(_top_count EQUAL 0)
    message(FATAL_ERROR "stage_libc_headers: no top-level *.h found in ${SRC}")
endif()
foreach(_h IN LISTS _top_headers)
    file(COPY_FILE "${SRC}/${_h}" "${DST}/${_h}" ONLY_IF_DIFFERENT)
endforeach()

# --- llvm-libc-types/ and llvm-libc-macros/ (flat) --------------------------
foreach(_sub llvm-libc-types llvm-libc-macros)
    if(IS_DIRECTORY "${SRC}/${_sub}")
        file(MAKE_DIRECTORY "${DST}/${_sub}")
        file(GLOB _headers RELATIVE "${SRC}/${_sub}" "${SRC}/${_sub}/*.h")
        foreach(_h IN LISTS _headers)
            file(COPY_FILE "${SRC}/${_sub}/${_h}" "${DST}/${_sub}/${_h}" ONLY_IF_DIFFERENT)
        endforeach()
    endif()
endforeach()

# --- llvm-libc-macros/baremetal/ -------------------------------------------
if(IS_DIRECTORY "${SRC}/llvm-libc-macros/baremetal")
    file(MAKE_DIRECTORY "${DST}/llvm-libc-macros/baremetal")
    file(GLOB _bm_headers RELATIVE "${SRC}/llvm-libc-macros/baremetal"
        "${SRC}/llvm-libc-macros/baremetal/*.h")
    foreach(_h IN LISTS _bm_headers)
        file(COPY_FILE
            "${SRC}/llvm-libc-macros/baremetal/${_h}"
            "${DST}/llvm-libc-macros/baremetal/${_h}"
            ONLY_IF_DIFFERENT)
    endforeach()
endif()
