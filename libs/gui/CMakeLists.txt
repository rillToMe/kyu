# ============================================================================
# libs/gui/CMakeLists.txt
# ============================================================================

add_subdirectory(color)
add_subdirectory(widget)

# libdesktop is the C++ desktop framework (archived, consumed by desktop.elf
# and the fileman/settings/taskmgr/browser apps).
if(KYUZEN_BUILD_LIBC_SDK)
    add_subdirectory(libdesktop)
endif()
