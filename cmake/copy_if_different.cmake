# ============================================================================
# copy_if_different.cmake — copy a file only when its content differs.
#
# The old Makefile used `cmp -s $< $@ || cp $< $@` for two compatibility
# copies (build/myos.bin and the Rust ELFs). The point is not politeness: an
# unconditional copy updates the mtime, which makes every downstream target
# (the ISO) look stale and rebuild on every invocation.
#
#   cmake -DSRC=<src> -DDST=<dst> -P copy_if_different.cmake
# ============================================================================

if(NOT SRC OR NOT DST)
    message(FATAL_ERROR "copy_if_different: both -DSRC and -DDST are required")
endif()

if(NOT EXISTS "${SRC}")
    message(FATAL_ERROR "copy_if_different: source does not exist: ${SRC}")
endif()

set(_needs_copy TRUE)

if(EXISTS "${DST}")
    file(SHA256 "${SRC}" _src_hash)
    file(SHA256 "${DST}" _dst_hash)
    if(_src_hash STREQUAL _dst_hash)
        set(_needs_copy FALSE)
    endif()
endif()

if(_needs_copy)
    get_filename_component(_dst_dir "${DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dst_dir}")
    file(COPY_FILE "${SRC}" "${DST}" ONLY_IF_DIFFERENT)
endif()
