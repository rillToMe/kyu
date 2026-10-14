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
# baremetal_common.cmake) for the KyuzenOS target triple.
#
# FPU/SSE IS ENABLED HERE. The archive is user-space code (it is linked into
# ring-3 ELFs), and user-space FPU state IS preserved across tasks: fpu_switch()
# (kernel/sched/fpu.c) runs on every context switch with FXSAVE/FXRSTOR.
#
# The previous flags carried -mno-sse -mno-sse2 -mno-mmx -msoft-float, which
# made every libc function that takes or returns float/double fail to compile
# ("SSE register return with SSE disabled"). That is why libm, difftime and the
# rest of <math.h> were absent from the SDK. With the flags gone they compile,
# and the functions the Kyuzen libc config already lists become usable.
#
# NOTE: the KERNEL is the opposite case and keeps -mno-sse (KYUZEN_KERNEL_FP_BAN
# in cmake/KyuzenCompiler.cmake). Interrupt entry does not save XMM, so kernel
# FP code would corrupt a running ring-3 task. This file is user-space only.
#
# -mno-red-zone stays: it is an ABI requirement, not an FPU decision.

set(CMAKE_SYSTEM_PROCESSOR x86_64 CACHE STRING "")
set(RUNTIMES_TARGET_TRIPLE "x86_64-pc-none-elf" CACHE STRING "")

foreach(lang C;CXX;ASM)
    set(CMAKE_${lang}_FLAGS "-mno-red-zone" CACHE STRING "")
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
