# ============================================================================
# libs/CMakeLists.txt
# ============================================================================

add_subdirectory(gui)
add_subdirectory(core)
add_subdirectory(media)
add_subdirectory(text)

if(KYUZEN_BUILD_LIBC_SDK)
    add_subdirectory(c)
    add_subdirectory(cpp)
endif()
