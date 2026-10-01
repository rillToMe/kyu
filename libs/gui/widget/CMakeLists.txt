# ============================================================================
# libs/gui/widget/CMakeLists.txt — libui widget toolkit (37 C++ sources).
#
# Built as an OBJECT library (not a static archive) because the old
# apps/Makefile linked these objects directly into every GUI app rather than
# through an archive. Object libraries also preserve the "no global
# constructors" property: apps/app.ld uses ENTRY(main) and never walks
# .init_array, so the toolkit must not rely on static initialisation.
#
# operator new/delete and __cxa_pure_virtual are stubbed ONCE in
# src/runtime/runtime.cpp. Do not define them anywhere else (ODR).
# ============================================================================

set(_widget_dir "${KYUZEN_ROOT}/libs/gui/widget")

# All toolkit layers + the C ABI shim. CONFIGURE_DEPENDS so a new layer
# directory is picked up on reconfigure (the old Makefile used
# $(wildcard $(WIDGET_DIR)/src/*/*.cpp)).
file(GLOB _widget_sources CONFIGURE_DEPENDS
    "${_widget_dir}/src/*/*.cpp"
)
list(APPEND _widget_sources "${_widget_dir}/abi/libui_abi.cpp")

add_library(kyuzen-widget OBJECT ${_widget_sources})
target_link_libraries(kyuzen-widget PRIVATE kyuzen-flags-widget)
target_include_directories(kyuzen-widget PUBLIC
    ${_widget_dir}/include
    ${KYUZEN_ROOT}/include
    ${KYUZEN_ROOT}/libs/gui/color/include
)
