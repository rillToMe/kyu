# ============================================================================
# libs/media/CMakeLists.txt — shared media helpers (user-space only).
#
#   kyuzen-png    PNG + BMP decoder (libs/media/png.c)
#   kyuzen-media  type detection / path utils / directory scan / header probe
#
# NEITHER is in the kernel image. media.c is entirely syscall-driven, so
# compiling it into the kernel would only produce dead code — the old Makefile
# excluded it from C_SOURCES for exactly that reason.
# ============================================================================

set(_media_dir "${KYUZEN_ROOT}/libs/media")

add_library(kyuzen-png OBJECT "${_media_dir}/png.c")
target_link_libraries(kyuzen-png PRIVATE kyuzen-flags-applib)
target_include_directories(kyuzen-png PUBLIC
    ${KYUZEN_ROOT}/include
    ${KYUZEN_ROOT}/libs/gui/color/include
)

add_library(kyuzen-media OBJECT "${_media_dir}/media.c")
target_link_libraries(kyuzen-media PRIVATE kyuzen-flags-applib)
target_include_directories(kyuzen-media PUBLIC
    ${KYUZEN_ROOT}/include
    ${KYUZEN_ROOT}/libs/gui/color/include
)
