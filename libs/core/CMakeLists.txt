# ============================================================================
# libs/core/CMakeLists.txt — shared user-space runtime pieces.
#
# Four independent object libraries, because different apps link different
# subsets (the old apps/Makefile expressed this with USERLIB_OBJ,
# USERUTIL_OBJ, NETUTIL_OBJ, LIBGUI_OBJ and listed them per app group):
#
#   kyuzen-userlib   syscall wrappers          -> every user-space ELF
#   kyuzen-userutil  UID -> username helpers   -> shell, login, terminal
#   kyuzen-netutil   reusable net client       -> shell, terminal, desktop
#   kyuzen-libgui    legacy GUI framework      -> GUI apps, terminal, desktop
#
# NONE of these are in the kernel image: userlib.c issues `int $0x80`, which
# is meaningless at CPL0. The old Makefile excluded all four from C_SOURCES.
# ============================================================================

set(_core_dir "${KYUZEN_ROOT}/libs/core")

# --- syscall wrappers (int $0x80) -------------------------------------------
add_library(kyuzen-userlib OBJECT "${_core_dir}/userlib.c")
target_link_libraries(kyuzen-userlib PRIVATE kyuzen-flags-applib)

# --- identity helpers (UID -> username) -------------------------------------
add_library(kyuzen-userutil OBJECT "${_core_dir}/userutil.c")
target_link_libraries(kyuzen-userutil PRIVATE kyuzen-flags-applib)

# --- reusable net client helpers (resolve / dial) ---------------------------
add_library(kyuzen-netutil OBJECT "${_core_dir}/netutil.c")
target_link_libraries(kyuzen-netutil PRIVATE kyuzen-flags-applib)

# --- legacy GUI framework (font8x16, drawing primitives) --------------------
add_library(kyuzen-libgui OBJECT "${_core_dir}/libgui.c")
target_link_libraries(kyuzen-libgui PRIVATE kyuzen-flags-applib)

foreach(_t kyuzen-userlib kyuzen-userutil kyuzen-netutil kyuzen-libgui)
    target_include_directories(${_t} PUBLIC ${KYUZEN_ROOT}/include)
endforeach()
