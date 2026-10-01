# ============================================================================
# iso_module_guard.cmake — verify every limine.conf module is present in the
# ISO staging directory.
#
# WHY THIS EXISTS
#   limine.conf declares each boot module with
#       module_path: boot():/NAME
#   If NAME is not on the ISO, Limine panics at BOOT with
#       "Failed to open module with path"
#   That is a runtime failure with no build-time signal. It has happened:
#   neofect.json was listed in limine.conf but never staged, so the ISO
#   panicked on every boot until someone traced it.
#
#   This guard converts that class of bug into a build error that names the
#   missing files.
#
# The old Makefile implemented this with a POSIX shell loop using `grep -o`,
# `sed`, and `[ -e ]`. This version is CMake-only, so it runs on any host
# without grep/sed/bash.
#
#   cmake -DISO_ROOT=<dir> -DLIMINE_CONF=<path> -P iso_module_guard.cmake
# ============================================================================

if(NOT ISO_ROOT OR NOT LIMINE_CONF)
    message(FATAL_ERROR "iso_module_guard: ISO_ROOT and LIMINE_CONF are required")
endif()

if(NOT IS_DIRECTORY "${ISO_ROOT}")
    message(FATAL_ERROR "iso_module_guard: staging directory not found: ${ISO_ROOT}")
endif()

if(NOT EXISTS "${LIMINE_CONF}")
    message(FATAL_ERROR "iso_module_guard: limine.conf not found: ${LIMINE_CONF}")
endif()

file(READ "${LIMINE_CONF}" _conf)

# ---------------------------------------------------------------------------
# Collect every `boot():/PATH` reference.
#
# The pattern stops at whitespace, matching the old `grep -o 'boot():/[^ ]*'`.
# Comments are not special-cased: the old guard did not skip them either, and
# a commented-out module_path is not present in practice.
# ---------------------------------------------------------------------------
string(REGEX MATCHALL "boot\\(\\):/[^ \t\r\n]+" _refs "${_conf}")

list(LENGTH _refs _ref_count)
if(_ref_count EQUAL 0)
    message(FATAL_ERROR
        "iso_module_guard: no module_path entries found in ${LIMINE_CONF}. "
        "Refusing to build an ISO with no modules — the config is probably wrong.")
endif()

set(_missing "")
set(_checked 0)

foreach(_ref IN LISTS _refs)
    # Strip the "boot():/" prefix to get the on-ISO path.
    string(REGEX REPLACE "^boot\\(\\):/" "" _path "${_ref}")

    # The kernel is referenced as kernel_path, not module_path, but if it ever
    # appears here it must also exist.
    if(NOT EXISTS "${ISO_ROOT}/${_path}")
        list(APPEND _missing "${_path}")
    endif()
    math(EXPR _checked "${_checked} + 1")
endforeach()

if(_missing)
    list(LENGTH _missing _missing_count)
    message(FATAL_ERROR
        "iso_module_guard: ${_missing_count} of ${_checked} modules referenced by "
        "${LIMINE_CONF} are missing from the ISO staging directory.\n"
        "Limine would PANIC at boot with 'Failed to open module with path'.\n\n"
        "Missing:\n"
        "  ${_missing}\n\n"
        "Fix by adding the file to the appropriate asset list in "
        "cmake/KyuzenImage.cmake (DESKTOP_ASSETS / FONT_ASSETS / SHELL_ASSETS), "
        "or to the app target list if it is an ELF.")
endif()

message(STATUS "ISO module guard: all ${_checked} module_path entries present")
