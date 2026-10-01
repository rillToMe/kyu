# ============================================================================
# libs/text/CMakeLists.txt — font manager + FreeType raster backend.
#
#   kyuzen-text  kzfont.c (manager, UTF-8, glyph cache) + kzraster_ft.c
#                (real rasteriser, needs the FreeType archive at link time)
#
# kzraster_ft.c is compiled WITH the FreeType port defines
# (-DKZFONT_USE_FREETYPE) because it calls into FreeType. kzfont.c is compiled
# without them so it also works with the mock raster backend used by
# tests/host/unit/text_test.
#
# The FreeType archive itself is NOT compiled here — it is linked at the final
# link step (as before).
# ============================================================================

set(_text_dir "${KYUZEN_ROOT}/libs/text")

# --- font manager (no FreeType dependency) ----------------------------------
add_library(kyuzen-text-manager OBJECT "${_text_dir}/src/kzfont.c")
target_link_libraries(kyuzen-text-manager PRIVATE kyuzen-flags-applib)
target_include_directories(kyuzen-text-manager PUBLIC
    ${_text_dir}/include
    ${KYUZEN_ROOT}/libs/gui/color/include
)

# --- FreeType-backed rasteriser ---------------------------------------------
add_library(kyuzen-text-raster OBJECT "${_text_dir}/src/kzraster_ft.c")
target_link_libraries(kyuzen-text-raster PRIVATE kyuzen-flags-freetype)
target_include_directories(kyuzen-text-raster PUBLIC
    ${_text_dir}/include
    ${KYUZEN_ROOT}/libs/gui/color/include
)

# Convenience grouping used by the apps that need both halves.
add_library(kyuzen-text INTERFACE)
target_link_libraries(kyuzen-text INTERFACE
    kyuzen-text-manager
    kyuzen-text-raster
    kyuzen-freetype
)
