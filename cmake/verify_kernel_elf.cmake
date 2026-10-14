# ============================================================================
# verify_kernel_elf.cmake — the kernel must contain no FP/SSE instructions.
#
#   cmake -DELF=<kernel> -DKYUZEN_OBJDUMP=<llvm-objdump>
#         -P cmake/verify_kernel_elf.cmake
#
# WHY THIS GUARD EXISTS (and why it is the KERNEL only)
#
#   FPU state is saved and restored per task by fpu_switch() in
#   kernel/sched/fpu.c, and that runs ONLY on a context switch. Interrupt entry
#   does NOT save XMM: arch/x86/isr_macro.inc pushes the general-purpose
#   registers only, and nothing else under arch/ or kernel/ touches fxsave.
#
#   So kernel code that emitted SSE would corrupt a running ring-3 task's XMM
#   registers whenever a timer interrupt landed inside it, and the task would
#   resume with wrong FP values. The failure is data-dependent and silent.
#
#   This is not hypothetical. kernel/string.c compiled without -mno-sse emits
#   `movups %xmm0` inside memcpy, and memcpy is reachable from scheduler and
#   filesystem paths that do run inside interrupts.
#
#   Userspace is the opposite case: fpu_switch() DOES run between tasks, so
#   user code may use float/double freely. That is why this check applies to
#   the kernel image and NOT to user ELFs — see cmake/verify_user_elf.cmake,
#   which no longer bans SSE.
#
#   The ban is a compiler-flag decision (KYUZEN_KERNEL_FP_BAN in
#   cmake/KyuzenCompiler.cmake). This script is the enforcement: if the flag is
#   ever dropped from a kernel target, the build fails here instead of
#   producing a system that corrupts FP state under interrupt load.
# ============================================================================

if(NOT DEFINED ELF)
    message(FATAL_ERROR "verify_kernel_elf: ELF is required")
endif()
if(NOT DEFINED KYUZEN_OBJDUMP)
    message(FATAL_ERROR "verify_kernel_elf: KYUZEN_OBJDUMP is required")
endif()
if(NOT EXISTS "${ELF}")
    message(FATAL_ERROR "verify_kernel_elf: ${ELF} does not exist")
endif()

execute_process(
    COMMAND ${KYUZEN_OBJDUMP} -d "${ELF}"
    OUTPUT_VARIABLE objdump_out
    ERROR_VARIABLE  objdump_err
    RESULT_VARIABLE objdump_rc
)
if(NOT objdump_rc EQUAL 0)
    message(FATAL_ERROR
        "verify_kernel_elf: llvm-objdump failed on ${ELF}: ${objdump_err}")
endif()

# ---------------------------------------------------------------------------
# SSE / AVX / AVX-512 vector registers.
#
# Match any vector register operand. `%xmm0` .. `%xmm31`, `%ymm*`, `%zmm*`.
# ---------------------------------------------------------------------------
string(REGEX MATCH "%(xmm|ymm|zmm)[0-9]+" sse_hit "${objdump_out}")
if(sse_hit)
    message(FATAL_ERROR
        "verify_kernel_elf: the KERNEL contains SSE/AVX instructions "
        "(${sse_hit}).\n"
        "\n"
        "The kernel must be built with -mno-sse -mno-sse2 -mno-mmx "
        "-msoft-float (KYUZEN_KERNEL_FP_BAN in cmake/KyuzenCompiler.cmake).\n"
        "\n"
        "Why this is fatal: interrupt entry does NOT save XMM "
        "(arch/x86/isr_macro.inc pushes general-purpose registers only), and "
        "fpu_switch() runs only on a context switch. Kernel SSE would "
        "therefore clobber a running ring-3 task's FP registers whenever an "
        "interrupt landed inside that code, and the task would resume with "
        "corrupted values.\n"
        "\n"
        "Fix: find the target that lost the flag set. Every kernel object "
        "library must link kyuzen-flags-kernel (or a set derived from it).")
endif()

# ---------------------------------------------------------------------------
# x87 stack instructions.
#
# -msoft-float already implies -mno-80387, so this is a second lock on the same
# door: it catches a flag set that passed -mno-sse but forgot -msoft-float.
# ---------------------------------------------------------------------------
string(REGEX MATCH
    "[ \t](fld|fst|fstp|fxch|fucom|fucomi|fadd|fsub|fmul|fdiv|fild|fist|fistp|fcom|fcomp)[ \t]"
    x87_hit "${objdump_out}")
if(x87_hit)
    message(FATAL_ERROR
        "verify_kernel_elf: the KERNEL contains x87 instructions (${x87_hit}).\n"
        "It must be built -msoft-float (KYUZEN_KERNEL_FP_BAN).")
endif()

message(STATUS "verify_kernel_elf: ${ELF} is free of FP/SSE instructions")
