# ============================================================================
# iso_stage.cmake — ISO staging helpers that need real logic.
#
#   cmake -DISO_ROOT=<dir> -DMODE=sweep    -P iso_stage.cmake
#   cmake -DISO_ROOT=<dir> -DMODE=optional "-DOPTIONAL_FILES=<;list>" -P iso_stage.cmake
#
# Why a script: the main recipe uses `cmake -E copy_if_different` for plain
# copies, but two steps need control flow.
# ============================================================================

if(NOT ISO_ROOT OR NOT MODE)
    message(FATAL_ERROR "iso_stage: ISO_ROOT and MODE are required")
endif()

if(MODE STREQUAL "sweep")
    # -----------------------------------------------------------------------
    # Remove stale ELFs from the staging directory.
    #
    # The old recipe ran `rm -f $(ISO_ROOT)/*.elf` so that an application that
    # was deleted or renamed does not linger in the image and get loaded by a
    # stale limine.conf entry.
    # -----------------------------------------------------------------------
    file(GLOB _stale_elfs "${ISO_ROOT}/*.elf")
    if(_stale_elfs)
        file(REMOVE ${_stale_elfs})
    endif()

elseif(MODE STREQUAL "optional")
    # -----------------------------------------------------------------------
    # Copy optional smoke-test ELFs if they exist, renaming each to the module
    # name limine.conf expects.
    #
    # The old Makefile had one `if [ -f $(APP) ]; then cp $(APP) $(ISO_ROOT)/<name>.elf; fi`
    # per smoke app. The list arrives as
    #   "src1=<path1>;src2=<path2>;..."  via OPTIONAL_FILES.
    # -----------------------------------------------------------------------
    foreach(_entry IN LISTS OPTIONAL_FILES)
        if(_entry MATCHES "^(.+)=(.+)$")
            set(_name "${CMAKE_MATCH_1}")
            set(_src  "${CMAKE_MATCH_2}")
            if(EXISTS "${_src}")
                file(COPY_FILE "${_src}" "${ISO_ROOT}/${_name}.elf" ONLY_IF_DIFFERENT)
            endif()
        endif()
    endforeach()

else()
    message(FATAL_ERROR "iso_stage: unknown MODE '${MODE}' (expected sweep|optional)")
endif()
