# ============================================================================
# libs/render/CMakeLists.txt - R1 native 2D software reference (libs/render).
#
#   kyuzen-render  kz_render.c (software backend-independent 2D API impl)
#
# Userspace + host-test library: allocates its context via malloc (kernel
# keeps its own draw path in kernel/gfx/). Integer-only, no per-primitive
# allocation, stride/format validated at every entry point.
# ============================================================================

set(_render_dir "${KYUZEN_ROOT}/libs/render")

add_library(kyuzen-render OBJECT "${_render_dir}/src/kz_render.c"
                                     "${_render_dir}/src/kz_shadow.c")
target_link_libraries(kyuzen-render PRIVATE kyuzen-flags-applib)
target_include_directories(kyuzen-render PUBLIC ${KYUZEN_ROOT}/include)
