# ============================================================================
# libs/gui/widget/CMakeLists.txt — libui widget toolkit (37 C++ sources).
#
# Built as OBJECT libraries (not a static archive) because the old
# apps/Makefile linked these objects directly into every GUI app rather than
# through an archive. Object libraries also preserve the "no global
# constructors" property: apps/app.ld uses ENTRY(main) and never walks
# .init_array, so the toolkit must not rely on static initialisation.
#
# operator new/delete and __cxa_pure_virtual are stubbed ONCE in
# src/runtime/runtime.cpp. Do not define them anywhere else (ODR).
#
# ---------------------------------------------------------------------------
# TWO TARGETS, TWO ORDERINGS — this is not redundancy.
#
# The old build linked the toolkit in two DIFFERENT orders depending on the
# app group, and object order determines symbol addresses:
#
#   GUI apps (apps/Makefile, GUI_LIBS/TERMINAL_LIBS):
#       WIDGET_OBJS = $(wildcard src/*/*.cpp) abi/libui_abi.cpp
#     -> src/** sorted, THEN abi
#
#   SDK apps (root Makefile, `$$(ls ...)` toolkit globs):
#       ls sorts its arguments, so color/src/*.o and widget/abi/*.o come
#       before widget/src/*.o
#     -> color_utils.o, abi/libui_abi.o, src/** sorted
#
# So the two halves are separate OBJECT libraries and each consumer lists them
# in the order its own recipe used. kyuzen-widget is the convenience INTERFACE
# for the GUI-app ordering.
# ---------------------------------------------------------------------------

set(_widget_dir "${KYUZEN_ROOT}/libs/gui/widget")

# --- layer sources (sorted) -------------------------------------------------
# kyuzen_glob resolves the pattern at configure time (the old Makefile used
# $(wildcard $(WIDGET_DIR)/src/*/*.cpp)). See cmake/KyuzenGlob.cmake for why
# CONFIGURE_DEPENDS is deliberately avoided.
kyuzen_glob(_widget_layer_sources
    "${_widget_dir}/src/*/*.cpp"
)
list(SORT _widget_layer_sources)
# TIPOGRAFI SISTEM DIKELUARKAN DARI GLOB.
# src/services/uifont.cpp memanggil kz_font_* dan karenanya butuh arsip FreeType
# saat link. Toolkit inti SENGAJA tidak men-link FreeType, jadi file itu tidak
# boleh masuk glob ini — kalau masuk, SETIAP aplikasi toolkit ikut menyeret
# FreeType dan invarian arsitekturnya rusak. Ia dibangun sebagai objek terpisah
# (kyuzen-widget-uifont) yang hanya di-link aplikasi yang memang mau tipografi.
list(FILTER _widget_layer_sources EXCLUDE REGEX "src/services/uifont\\.cpp$")

add_library(kyuzen-widget-src OBJECT ${_widget_layer_sources})
target_link_libraries(kyuzen-widget-src PRIVATE kyuzen-flags-widget)

# --- font UI sistem (OPSIONAL) ----------------------------------------------
# Di-link eksplisit oleh aplikasi yang ingin teks toolkit memakai font pilihan
# pengguna (font.ui). Butuh: kyuzen-text-manager kyuzen-text-raster kyuzen-freetype.
add_library(kyuzen-widget-uifont OBJECT "${_widget_dir}/src/services/uifont.cpp")
target_link_libraries(kyuzen-widget-uifont PRIVATE kyuzen-flags-widget)
target_include_directories(kyuzen-widget-uifont PRIVATE
    ${KYUZEN_ROOT}/libs/text/include
)

# --- C ABI shim -------------------------------------------------------------
add_library(kyuzen-widget-abi OBJECT "${_widget_dir}/abi/libui_abi.cpp")
target_link_libraries(kyuzen-widget-abi PRIVATE kyuzen-flags-widget)

foreach(_t kyuzen-widget-src kyuzen-widget-abi kyuzen-widget-uifont)
    target_include_directories(${_t} PUBLIC
        ${_widget_dir}/include
        ${KYUZEN_ROOT}/include
        ${KYUZEN_ROOT}/libs/gui/color/include
    )
endforeach()

# --- GUI-app ordering: src first, then abi ----------------------------------
add_library(kyuzen-widget INTERFACE)
target_link_libraries(kyuzen-widget INTERFACE
    kyuzen-widget-src
    kyuzen-widget-abi
)
