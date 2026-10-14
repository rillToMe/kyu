# ============================================================================
# verify_user_elf.cmake — post-build validation of a user-space ELF.
#
# Reproduces the sanity greps the old Makefile ran after linking every SDK-built
# ELF (root Makefile lines ~1572-1585, 2080-2086, 2128-2134, 2175-2181,
# 2288-2294). Those checks exist because these failures are silent at build
# time and only show up as a crash or a hang at boot.
#
# Invoked with:
#   -DELF=<path> -DKYUZEN_NM=<llvm-nm> -DKYUZEN_OBJDUMP=<llvm-objdump>
#   [-DEXPECTED_ENTRY=_start|main]
#
# Checked here: entry point, no unresolved symbols, no exception/unwind/thread
# runtime, and no AVX (see check 4 — SSE and x87 are deliberately allowed in
# user space, and banned in the kernel by cmake/verify_kernel_elf.cmake).
#
# Written as a CMake script (not a shell pipeline) so it runs on any host
# without grep/sed/bash.
# ============================================================================

if(NOT EXISTS "${ELF}")
    message(FATAL_ERROR "verify_user_elf: ELF not found: ${ELF}")
endif()

# ---------------------------------------------------------------------------
# Full symbol table (used by checks 1 and 3).
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${KYUZEN_NM} "${ELF}"
    OUTPUT_VARIABLE nm_all
    ERROR_VARIABLE  nm_err
    RESULT_VARIABLE nm_rc
)
if(NOT nm_rc EQUAL 0)
    message(FATAL_ERROR "verify_user_elf: llvm-nm failed on ${ELF}: ${nm_err}")
endif()

# ---------------------------------------------------------------------------
# 1. Entry point.
# ---------------------------------------------------------------------------
if(EXPECTED_ENTRY)
    string(REGEX MATCH "[ \t][Tt] ${EXPECTED_ENTRY}(\r?\n|$)" entry_hit "${nm_all}")
    if(NOT entry_hit)
        message(FATAL_ERROR
            "verify_user_elf: entry symbol '${EXPECTED_ENTRY}' not found in ${ELF}")
    endif()
endif()

# ---------------------------------------------------------------------------
# 2. No unresolved symbols.
#
# `llvm-nm --undefined-only` lists symbols this object references but does not
# define. For a statically linked freestanding ELF the list must be empty;
# anything left is a missing archive member or a wrong link order.
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${KYUZEN_NM} --undefined-only "${ELF}"
    OUTPUT_VARIABLE nm_undef
    ERROR_VARIABLE  nm_undef_err
    RESULT_VARIABLE nm_undef_rc
)
if(NOT nm_undef_rc EQUAL 0)
    message(FATAL_ERROR "verify_user_elf: llvm-nm --undefined-only failed: ${nm_undef_err}")
endif()
string(STRIP "${nm_undef}" nm_undef_stripped)
if(NOT nm_undef_stripped STREQUAL "")
    message(FATAL_ERROR
        "verify_user_elf: ${ELF} has unresolved symbols:\n${nm_undef}")
endif()

# ---------------------------------------------------------------------------
# 3. No exception / unwinding / thread runtime.
#
# These apps have no C++ runtime: -fno-exceptions -fno-rtti, and there is no
# unwinder or pthread implementation on this target. Their presence means the
# link accidentally pulled in hosted libc++ or a wrong archive.
# ---------------------------------------------------------------------------
string(REGEX MATCH
    "[ \t](__cxa_throw|__cxa_begin_catch|__cxa_end_catch|_Unwind_[A-Za-z0-9_]*|__gxx_personality_[A-Za-z0-9_]*|pthread_[A-Za-z0-9_]*)(\r?\n|$)"
    cxa_hit "${nm_all}")
if(cxa_hit)
    message(FATAL_ERROR
        "verify_user_elf: ${ELF} pulls in exception/unwind/thread runtime: ${cxa_hit}")
endif()

# ---------------------------------------------------------------------------
# 4. FPU instruction policy: XMM and x87 are ALLOWED; AVX (ymm/zmm) is NOT.
#
# WHY THIS IS NO LONGER A BLANKET BAN
#
#   User-space FPU state IS preserved across tasks: fpu_switch()
#   (kernel/sched/fpu.c) runs on every context switch and uses FXSAVE/FXRSTOR.
#   So user code may freely use float/double — that is what opened the C/C++
#   SDK's <math.h>, libm, and the double-returning libc functions.
#
#   FXSAVE saves exactly x87 + XMM0..XMM15 + MXCSR. It does NOT save the upper
#   halves of YMM/ZMM — those need XSAVE, which this kernel does not use (see
#   the XSAVE-vs-FXSAVE reasoning in kernel/sched/fpu.c). An AVX instruction in
#   a user ELF would therefore have its upper 128 bits silently clobbered by
#   the next task, producing wrong results that depend on scheduling order.
#
#   So the boundary is not "FP vs no FP", it is "FXSAVE-covered vs not":
#
#     xmm, x87   allowed  — covered by FXSAVE/FXRSTOR
#     ymm, zmm   banned   — NOT covered; needs XSAVE
#
#   The default -march for this target is baseline x86-64 (SSE2, no AVX), so
#   nothing emits ymm today. The check is here so that stays true if a -march
#   is ever raised, or if a vendored library ships precompiled AVX code.
#
#   The KERNEL has the opposite rule — no FP at all — because interrupt entry
#   does not save XMM. That is enforced separately by
#   cmake/verify_kernel_elf.cmake.
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${KYUZEN_OBJDUMP} -d "${ELF}"
    OUTPUT_VARIABLE objdump_out
    ERROR_VARIABLE  objdump_err
    RESULT_VARIABLE objdump_rc
)
if(NOT objdump_rc EQUAL 0)
    message(FATAL_ERROR "verify_user_elf: llvm-objdump failed on ${ELF}: ${objdump_err}")
endif()

string(REGEX MATCH "%(ymm|zmm)[0-9]+" avx_hit "${objdump_out}")
if(avx_hit)
    message(FATAL_ERROR
        "verify_user_elf: ${ELF} contains AVX instructions (${avx_hit}).\n"
        "\n"
        "The kernel saves FPU state with FXSAVE/FXRSTOR, which covers x87 and "
        "XMM0-15 only. AVX's upper YMM/ZMM halves are NOT saved, so this "
        "component's FP state would be corrupted by the next task in a way "
        "that depends on scheduling order.\n"
        "\n"
        "Fix: build this target with a baseline x86-64 -march (the default), "
        "or extend the kernel to XSAVE before enabling AVX.")
endif()
