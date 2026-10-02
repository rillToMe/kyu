# KyuzenOS — LLVM libc baremetal x86_64 cache
#
# Usage (standalone cross build, see libc/docs/full_cross_build.rst):
#
#   cmake -S <llvm-project>/runtimes -B <build-dir> \
#         -G "Unix Makefiles" \
#         -C <llvm-project>/libc/cmake/caches/x86_64-pc-none-elf.cmake \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#         -DCMAKE_BUILD_TYPE=Release
#
# The cache mirrors libc/cmake/caches/*-none-eabi.cmake (which include
# baremetal_common.cmake) for the KyuzenOS target triple. The compiler flags
# match the flags user_apps/Makefile already uses for Ring-3 code: the KyuzenOS
# kernel never enables SSE (no CR4.OSFXSR, no FXSAVE/FXRSTOR), so the archive
# must not contain SSE/MMX instructions either.

set(CMAKE_SYSTEM_PROCESSOR x86_64 CACHE STRING "")
set(RUNTIMES_TARGET_TRIPLE "x86_64-pc-none-elf" CACHE STRING "")

foreach(lang C;CXX;ASM)
    set(CMAKE_${lang}_FLAGS "-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float" CACHE STRING "")
endforeach()

# baremetal_common.cmake is UPSTREAM's, so it lives in the clone, not next to
# this file. The original used ${CMAKE_CURRENT_LIST_DIR}, which only worked
# because this cache file used to sit in that same directory.
#
# KYUZEN_LLVM_LIBC_CACHES is passed by libs/c/CMakeLists.txt. The fallback
# resolves the clone relative to this file, so the cache still works when
# invoked by hand with -C.
if(NOT DEFINED KYUZEN_LLVM_LIBC_CACHES)
    get_filename_component(KYUZEN_LLVM_LIBC_CACHES
        "${CMAKE_CURRENT_LIST_DIR}/../../../../llvm-project/libc/cmake/caches"
        ABSOLUTE)
endif()
include(${KYUZEN_LLVM_LIBC_CACHES}/baremetal_common.cmake)
