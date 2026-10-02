# ============================================================================
# KyuzenOS — top-level build (CMake + Ninja + Clang/LLD)
#
#   cmake -S . -B build -G Ninja
#   cmake --build build
#
# Host-side unit tests are a SEPARATE build (they must not be compiled with the
# bare-metal toolchain):
#
#   cmake -S tests/host -B build/host -G Ninja
#   ctest --test-dir build/host
#
# Design notes: BUILD_SYSTEM_AUDIT.md §14.  Migration report: CMAKE_MIGRATION.md.
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Toolchain: install KyuzenToolchain.cmake automatically.
#
# CMAKE_TOOLCHAIN_FILE is read by project() below, so it has to be set BEFORE
# that call — and it is only ever read from the cache. Setting it here means
# the documented `cmake -S . -B build -G Ninja` works on a clean tree.
#
# This is not just convenience. Without it, a FRESH configure silently picked
# the host compiler (project() ran with no toolchain, found c++.exe, and only
# failed later in an unrelated place — BearSSL's host-triple probe). Existing
# build directories kept working only because CMakeCache.txt had recorded the
# toolchain path during the very first configure, so the breakage was invisible
# to every incremental reconfigure and appeared only under `rm -rf build`.
#
# An explicit -DCMAKE_TOOLCHAIN_FILE=... still wins (the guard tests for it),
# and a non-empty cached value from an earlier configure is left untouched.
# ---------------------------------------------------------------------------
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/KyuzenToolchain.cmake"
        CACHE FILEPATH "Toolchain file for the KyuzenOS bare-metal target")
endif()

project(KyuzenOS
    VERSION     0.1.0
    DESCRIPTION "KyuzenOS — x86_64 higher-half bare-metal operating system"
    LANGUAGES   C CXX ASM_NASM
)

# Absolute source root. Every module refers to sources through this so that
# subdirectory CMakeLists.txt files never need ../.. relative paths.
set(KYUZEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Options — each one corresponds to a real build variation. See
# BUILD_SYSTEM_AUDIT.md §14.6 for why no others exist.
# ---------------------------------------------------------------------------
option(KYUZEN_BUILD_APPS     "Build user-space applications"           ON)
option(KYUZEN_BUILD_ISO      "Build the bootable ISO image"            ON)
option(KYUZEN_BUILD_RUST     "Build the Rust applications (cargo)"     ON)
option(KYUZEN_BUILD_LIBC_SDK "Build the LLVM libc C/C++ SDKs (slow)"   ON)
option(KYUZEN_ENABLE_INTEL_GHAL "Include the Intel Gen12 graphics backend" ON)

# ATA read path default. The old Makefile needed a stamp file for this
# (build/ata_read_path_default.stamp) because Make only compares timestamps and
# cannot see that a variable changed. CMake reconfigures when a cache variable
# changes, so the stamp is unnecessary.
set(KYUZEN_ATA_READ_PATH_DEFAULT 0 CACHE STRING "ATA read path: 0=LEGACY 1=BATCH")
set_property(CACHE KYUZEN_ATA_READ_PATH_DEFAULT PROPERTY STRINGS 0 1)

# Which implementation fills the desktop.elf boot slot.
set(KYUZEN_DESKTOP_APP "desktop" CACHE STRING "Desktop implementation directory under system/")

# LLVM libc internal namespace. MUST match the pinned libc version string, or
# the port layer will not interpose on libc's own symbols.
set(KYUZEN_LIBC_NAMESPACE "__llvm_libc_22_1_8_" CACHE STRING "LLVM libc internal namespace")

# QEMU executable. Bare name by default (matches the old Makefile); override
# when qemu-system-x86_64 is not on PATH.
set(KYUZEN_QEMU "qemu-system-x86_64" CACHE FILEPATH "QEMU system emulator executable")

# ---------------------------------------------------------------------------
# Output layout — everything generated lives under the build directory.
# Nothing is written into the source tree.
# ---------------------------------------------------------------------------
set(KYUZEN_BIN_DIR    "${CMAKE_BINARY_DIR}/bin")     # kernel
set(KYUZEN_ELF_DIR    "${CMAKE_BINARY_DIR}/apps")    # user-space ELFs
set(KYUZEN_LIB_DIR    "${CMAKE_BINARY_DIR}/lib")     # static libraries
set(KYUZEN_ISO_ROOT   "${CMAKE_BINARY_DIR}/iso_root")# ISO staging
set(KYUZEN_ISO_IMAGE  "${CMAKE_BINARY_DIR}/boot_image.iso")
set(KYUZEN_SDK_DIR    "${CMAKE_BINARY_DIR}/sdk")     # staged C / C++ SDKs
set(KYUZEN_LIBC_DIR   "${CMAKE_BINARY_DIR}/libc")    # nested LLVM libc build

# Native header dependency tracking (replaces the Makefile's -MMD -MP).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Canonical flag sets + kernel source lists + linker rule.
#
# The linker rule must be installed BEFORE any target is created, and
# KyuzenGlob must be available before any subdirectory globs its sources.
# ---------------------------------------------------------------------------
include(${KYUZEN_ROOT}/cmake/KyuzenLinker.cmake)
include(${KYUZEN_ROOT}/cmake/KyuzenGlob.cmake)
include(${KYUZEN_ROOT}/cmake/KyuzenCompiler.cmake)
include(${KYUZEN_ROOT}/cmake/KyuzenSources.cmake)
include(${KYUZEN_ROOT}/cmake/KyuzenApplication.cmake)
include(${KYUZEN_ROOT}/cmake/KyuzenImage.cmake)

# ---------------------------------------------------------------------------
# Target build.
#
# Order matters only for readability; real dependencies are declared on the
# targets themselves.
# ---------------------------------------------------------------------------
add_subdirectory(third_party)
add_subdirectory(libs)
add_subdirectory(kernel)

# Applications: the system utilities and the apps. Both append their target
# names to KYUZEN_APP_TARGETS, which the ISO module depends on.
set(KYUZEN_APP_TARGETS "")
set(KYUZEN_OPTIONAL_ELF_TARGETS "")

if(KYUZEN_BUILD_APPS)
    add_subdirectory(system)
    list(APPEND KYUZEN_APP_TARGETS ${KYUZEN_SYSTEM_APP_TARGETS})

    add_subdirectory(apps)
    list(APPEND KYUZEN_APP_TARGETS ${KYUZEN_APP_TARGETS_FROM_APPS})
endif()

# Rust stays a separate build domain (Rule 17): CMake invokes cargo and copies
# the resulting ELFs. Must run after apps/ so KYUZEN_ELF_DIR exists.
if(KYUZEN_BUILD_RUST)
    add_subdirectory(rust)
endif()

if(KYUZEN_BUILD_ISO)
    kyuzen_add_image_target()
endif()

# ---------------------------------------------------------------------------
# Convenience: build everything that the ISO needs.
# ---------------------------------------------------------------------------
add_custom_target(kyuzen-all
    DEPENDS kyuzen-kernel
    COMMENT "KyuzenOS: kernel (+ apps/ISO when enabled)"
)

if(KYUZEN_BUILD_ISO)
    add_dependencies(kyuzen-all kyuzen-image)
endif()

# ---------------------------------------------------------------------------
# QEMU run targets — only the workflows that already existed.
# ---------------------------------------------------------------------------
if(KYUZEN_BUILD_ISO)
    include(${KYUZEN_ROOT}/cmake/KyuzenQemu.cmake)
endif()
