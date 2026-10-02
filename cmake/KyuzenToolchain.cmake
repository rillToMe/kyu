# ============================================================================
# KyuzenToolchain.cmake — bare-metal toolchain for the KyuzenOS target build.
#
# Usage:
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/KyuzenToolchain.cmake
#
# This file configures the TARGET build only (kernel + user-space apps, all
# freestanding x86_64-pc-none-elf). Host tests use a SEPARATE CMake invocation
# with the host toolchain — see tests/host/CMakeLists.txt.
#
# Verified against the existing Makefile (see BUILD_SYSTEM_AUDIT.md §3):
#   CC = clang, LD = ld.lld, AS = nasm
#   triple = x86_64-pc-none-elf
# ============================================================================

# Generic (not Linux/Windows): there is no hosted runtime for this target.
# CMAKE_SYSTEM_NAME=Generic tells CMake not to expect crt0/libc and to skip
# link checks that would otherwise fail on a freestanding target.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# ---------------------------------------------------------------------------
# Compilers.
#
# Bare names on purpose (no hardcoded developer paths — the Makefile does the
# same). Override on the command line if a tool is not on PATH, e.g.:
#   -DCMAKE_C_COMPILER=/path/to/clang
#
# PATH ORDER MATTERS on MSYS2: the `usr/bin` (MSYS) toolchain and the
# `clang64/bin` (MINGW) toolchain are DIFFERENT LLVM versions. The project's
# documented setup and its verified baseline build use the MSYS one, so the
# baseline hash comparison depends on resolving the same compiler.
# ---------------------------------------------------------------------------
find_program(KYUZEN_CLANG    NAMES clang    REQUIRED)
find_program(KYUZEN_CLANGXX  NAMES clang++  REQUIRED)
find_program(KYUZEN_LD_LLD   NAMES ld.lld   REQUIRED)
find_program(KYUZEN_NASM     NAMES nasm     REQUIRED)

# Warn if the selected clang is not the version the baseline was measured
# with. Not fatal: a different LLVM may still produce equivalent output, but
# any binary difference must then be attributed to the toolchain, not to the
# migration.
set(KYUZEN_EXPECTED_CLANG_MAJOR "21" CACHE STRING
    "LLVM major version the parity baseline was produced with")

execute_process(
    COMMAND "${KYUZEN_CLANG}" --version
    OUTPUT_VARIABLE _clang_version_out
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
string(REGEX MATCH "clang version ([0-9]+)" _ "${_clang_version_out}")
if(CMAKE_MATCH_1 AND NOT CMAKE_MATCH_1 STREQUAL KYUZEN_EXPECTED_CLANG_MAJOR)
    message(WARNING
        "Selected C compiler is Clang ${CMAKE_MATCH_1} (${KYUZEN_CLANG}) but the "
        "parity baseline was produced with Clang ${KYUZEN_EXPECTED_CLANG_MAJOR}.\n"
        "Binary parity checks may differ because of the toolchain, not the build "
        "system. Put the expected toolchain first on PATH, or set "
        "-DKYUZEN_EXPECTED_CLANG_MAJOR=${CMAKE_MATCH_1} to acknowledge.")
endif()

# Binary-inspection tools used by the post-build ELF verification steps
# (cmake/verify_user_elf.cmake). The old Makefile used the same three.
find_program(KYUZEN_LLVM_NM       NAMES llvm-nm       REQUIRED)
find_program(KYUZEN_LLVM_OBJDUMP  NAMES llvm-objdump  REQUIRED)
find_program(KYUZEN_LLVM_AR       NAMES llvm-ar       REQUIRED)
find_program(KYUZEN_LLVM_OBJCOPY  NAMES llvm-objcopy  REQUIRED)

# ---------------------------------------------------------------------------
# DEPFILE PATH STYLE — normalise POSIX paths so native Ninja can stat them.
#
# The wrapper itself is shared with the host-test build (which needs the same
# treatment but must not include this toolchain file) — see
# cmake/KyuzenCompilerWrapper.cmake for the full rationale.
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/KyuzenCompilerWrapper.cmake")

kyuzen_install_compiler_wrapper("${KYUZEN_CLANG}" "${KYUZEN_CLANGXX}")

set(CMAKE_C_COMPILER   "${KYUZEN_WRAPPED_CC}")
set(CMAKE_CXX_COMPILER "${KYUZEN_WRAPPED_CXX}")

set(CMAKE_LINKER "${KYUZEN_LD_LLD}")

# NASM assembles the 12 arch/x86/*.asm files (Intel syntax). CMake has
# first-class NASM support via the ASM_NASM language; the language itself is
# enabled by the top-level project() call.
set(CMAKE_ASM_NASM_COMPILER "${KYUZEN_NASM}")
set(CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS asm)

# ---------------------------------------------------------------------------
# Target triple.
#
# The kernel and every user-space component compile with
# `--target=x86_64-pc-none-elf` (bare-metal ABI). Passing it through
# CMAKE_*_COMPILER_TARGET makes CMake bake it into compiler detection, the
# Ninja rules, and compile_commands.json.
# ---------------------------------------------------------------------------
set(CMAKE_C_COMPILER_TARGET   x86_64-pc-none-elf)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-none-elf)

# ---------------------------------------------------------------------------
# CRITICAL: never try to link+run a test executable.
#
# Compiler detection normally compiles AND links a small program, then runs it.
# A freestanding x86_64-pc-none-elf binary cannot link without a CRT and
# cannot run on the host. STATIC_LIBRARY makes the check stop after compiling
# to an object — enough to validate the compiler.
# ---------------------------------------------------------------------------
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ---------------------------------------------------------------------------
# No implicit anything.
#
# The Makefile passes -nostdlib on every link line and relies on no implicit
# libraries or startup files. Mirror that so CMake does not inject its own.
#
# IMPORTANT: -nostdlib is NOT set via CMAKE_EXE_LINKER_FLAGS_INIT, even though
# that would be the natural place. ld.lld only accepts `-flavor gnu` as its
# FIRST argument — once any other option is seen, it has already committed to
# a flavour and rejects -flavor. CMake places CMAKE_EXE_LINKER_FLAGS_INIT
# before target link options, which would put -nostdlib ahead of -flavor and
# break the link. Each target therefore passes -nostdlib itself, exactly as
# the old Makefile's per-recipe link lines did.
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD_LIBRARIES   "" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)

# Do not search host sysroots for headers/libraries.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
