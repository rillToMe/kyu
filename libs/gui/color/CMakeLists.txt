# ============================================================================
# libs/gui/color/CMakeLists.txt
#
# libs/gui/color is the ONLY source compiled into BOTH the kernel and
# user-space (docs/development/build-system-audit.md §7.1). Two targets, two flag sets, two
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
# User-space copy.
#
# SPLIT INTO TWO TARGETS ON PURPOSE. The old apps/Makefile compiled all three
# colour sources (COLOR_OBJS) but only ever LINKED one of them into apps:
#
#     LIBUI_COLOR_OBJS = $(OBJ_DIR)/libs/gui/color/src/color_utils.o
#     GUI_LIBS = ... $(PNG_OBJ) $(LIBUI_COLOR_OBJS)
#
# so color_blend.c and color_space.c were built and then not linked. The app
# link line therefore ends with `... png.o color_utils.o`, and reproducing it
# exactly is what keeps the ELFs byte-identical.
#
#   kyuzen-color-utils   the one object user-space apps actually link
#   kyuzen-color         all three (kept for completeness / future use)
# ---------------------------------------------------------------------------
set(_color_utils_src "${CMAKE_CURRENT_SOURCE_DIR}/src/color_utils.c")

add_library(kyuzen-color-utils OBJECT "${_color_utils_src}")
target_link_libraries(kyuzen-color-utils PRIVATE kyuzen-flags-applib)
target_include_directories(kyuzen-color-utils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

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
