#ifndef KYUZEN_MSR_H
#define KYUZEN_MSR_H

// ============================================================
// include/msr.h — akses MSR (Model Specific Register)
//
// Sebelumnya `rdmsr`/`wrmsr` adalah static helper lokal di arch/x86/lapic.c.
// Begitu ada konsumen kedua (EFER.NXE di kernel/cpu.c), salinan lokal jadi
// sumber drift. Header ini menjadikannya satu implementasi bersama.
// ============================================================

#include <stdint.h>

#define MSR_IA32_APIC_BASE 0x1B
#define MSR_IA32_EFER      0xC0000080

// ARCH-9: GS/FS base. IA32_GS_BASE dipakai kernel untuk per-CPU data;
// IA32_KERNEL_GS_BASE disediakan untuk swapgs (belum dipakai — lihat
// catatan di kernel/smp/smp.c).
#define MSR_IA32_GS_BASE        0xC0000101
#define MSR_IA32_KERNEL_GS_BASE 0xC0000102

// Bit EFER yang relevan (AMD64 APM Vol.2 / Intel SDM Vol.4)
#define EFER_SCE  (1ULL << 0)    // Syscall Enable
#define EFER_LME  (1ULL << 8)    // Long Mode Enable
#define EFER_LMA  (1ULL << 10)   // Long Mode Active (read-only)
#define EFER_NXE  (1ULL << 11)   // No-Execute Enable  <-- ARCH-5

static inline uint64_t kz_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void kz_wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

#endif // KYUZEN_MSR_H
