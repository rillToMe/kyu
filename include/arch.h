#ifndef KYUZEN_ARCH_H
#define KYUZEN_ARCH_H

// ============================================================
// include/arch.h — Kontrak antara arch/x86/ dan kernel portable.
//
// Header ini adalah SATU-SATUNYA tempat kernel mengetahui detail lapisan
// arsitektur yang tidak portable (vektor, IST, GDT selector). Kernel tidak
// boleh meng-hardcode angka-angka ini di tempat lain.
// ============================================================

#include <stdint.h>

// ------------------------------------------------------------
// Vektor exception (Intel SDM Vol.3 Table 6-1)
// ------------------------------------------------------------
#define KZ_VEC_DE      0    // Divide Error
#define KZ_VEC_DB      1    // Debug
#define KZ_VEC_NMI     2    // Non-Maskable Interrupt      (IST)
#define KZ_VEC_BP      3    // Breakpoint                  (DPL=3)
#define KZ_VEC_OF      4    // Overflow                    (DPL=3)
#define KZ_VEC_BR      5    // BOUND Range Exceeded        (DPL=3)
#define KZ_VEC_UD      6    // Invalid Opcode              (DPL=3)
#define KZ_VEC_NM      7    // Device Not Available
#define KZ_VEC_DF      8    // Double Fault                (IST)
#define KZ_VEC_TS     10    // Invalid TSS
#define KZ_VEC_NP     11    // Segment Not Present
#define KZ_VEC_SS     12    // Stack-Segment Fault
#define KZ_VEC_GP     13    // General Protection
#define KZ_VEC_PF     14    // Page Fault
#define KZ_VEC_AC     17    // Alignment Check
#define KZ_VEC_MC     18    // Machine Check               (IST)
#define KZ_VEC_XM     19    // SIMD Floating-Point
#define KZ_VEC_VE     20    // Virtualization Exception
#define KZ_VEC_CP     21    // Control Protection
#define KZ_VEC_VC     29    // VMM Communication
#define KZ_VEC_SX     30    // Security Exception

#define KZ_VEC_IRQ0   32    // PIT timer   (setelah PIC remap)
#define KZ_VEC_IRQ1   33    // Keyboard
#define KZ_VEC_IRQ12  44    // Mouse
#define KZ_VEC_SYSCALL 128  // int 0x80    (DPL=3)
#define KZ_VEC_LAPIC_TIMER     240  // 0xF0
#define KZ_VEC_LAPIC_RESCHED   253  // 0xFD
#define KZ_VEC_LAPIC_SPURIOUS  255  // 0xFF — diset di LAPIC SVR

// ------------------------------------------------------------
// IST (Interrupt Stack Table) — ARCH-2
//
// #DF, NMI, dan #MC WAJIB punya stack sendiri: fault-nya bisa terjadi justru
// ketika stack yang sedang dipakai korup/penuh (mis. #PF karena stack
// overflow → #DF). Tanpa IST, push frame berikutnya gagal → triple fault →
// reset tanpa BSOD.
//
// IST index 0 berarti "tidak pakai IST" (memakai RSP0/RSP lama).
// ------------------------------------------------------------
#define KZ_IST_NONE  0
#define KZ_IST_DF    1      // #DF  — stack paling kritis
#define KZ_IST_NMI   2      // NMI
#define KZ_IST_MC    3      // #MC
#define KZ_IST_COUNT 3

#define KZ_IST_STACK_SIZE 8192   // 8 KB per stack (cukup untuk panic path)

// ------------------------------------------------------------
// GDT selector (lihat arch/x86/gdt.c)
// ------------------------------------------------------------
#define KZ_SEL_KERNEL_CS 0x08
#define KZ_SEL_KERNEL_DS 0x10
#define KZ_SEL_USER_CS   0x1B   // GDT[3] | RPL3
#define KZ_SEL_USER_DS   0x23   // GDT[4] | RPL3

// ------------------------------------------------------------
// Batas virtual kernel (higher-half)
// ------------------------------------------------------------
#define KZ_KERNEL_VMA 0xFFFFFFFF80000000ULL

#endif // KYUZEN_ARCH_H
