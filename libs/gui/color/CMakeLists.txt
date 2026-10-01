# ============================================================================
# libs/gui/color/CMakeLists.txt
#
# libs/gui/color is the ONLY source compiled into BOTH the kernel and
# user-space (BUILD_SYSTEM_AUDIT.md §7.1). Two targets, two flag sets, two
# separate object files — they cannot share objects because the kernel copy
# needs -mcmodel=kernel and the user-space copy must not have it.
#
#   kyuzen-color-kernel  -> consumed by the kernel image
#   kyuzen-color         -> consumed by user-space apps
# ============================================================================

set(_color_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/src/color_blend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/color_space.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/color_utils.c"
)

# ---------------------------------------------------------------------------
# User-space copy (what apps/Makefile's COLOR_SRCS produced).
# ---------------------------------------------------------------------------
add_library(kyuzen-color OBJECT ${_color_sources})
target_link_libraries(kyuzen-color PRIVATE kyuzen-flags-applib)
target_include_directories(kyuzen-color PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Kernel copy.
#
# NOTE: the kernel target does NOT consume this object library. kernel/
# CMakeLists.txt compiles its own copy of the same three files inside
# kyuzen-kernel-color, because a single OBJECT library cannot carry two
# different flag sets. This target exists so the intent is documented and so
# anything else that needs "colour math with kernel flags" has a home.
# ---------------------------------------------------------------------------
add_library(kyuzen-color-kernel OBJECT ${_color_sources})
target_link_libraries(kyuzen-color-kernel PRIVATE kyuzen-flags-kernel)
target_include_directories(kyuzen-color-kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
