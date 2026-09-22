// ============================================================
// KERNEL CPU INFO — kernel/cpu.c
// Mengimplementasikan get_cpu_string() via instruksi CPUID.
// Dipanggil oleh syscall_handler (syscall 17) dan shell.c (fetch).
// FIX_005 Tahap 4: cpu_enable_smap_smep() + cpu_verify_wp().
// ============================================================
#include <stdint.h>
#include "smap.h"


int g_smap_enabled = 0;
int g_smep_enabled = 0;

#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)
#define CR0_WP   (1ULL << 16)

void cpu_enable_smap_smep(void) {
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile("cpuid" : "=a"(eax) : "a"(0) : "ebx", "ecx", "edx");
    if (eax < 7) return;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7), "c"(0));

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (ebx & (1u << 7))  cr4 |= CR4_SMEP;
    if (ebx & (1u << 20)) cr4 |= CR4_SMAP;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    g_smep_enabled = (ebx >> 7)  & 1;
    g_smap_enabled = (ebx >> 20) & 1;
}

int cpu_verify_wp(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    if (!(cr0 & CR0_WP)) {
        cr0 |= CR0_WP;
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    }
    return (cr0 & CR0_WP) ? 1 : 0;
}

void get_cpu_string(char* buffer) {
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0)
    );

    ((uint32_t*)buffer)[0] = ebx;
    ((uint32_t*)buffer)[1] = edx;
    ((uint32_t*)buffer)[2] = ecx;

    uint32_t max_ext;
    __asm__ volatile ("cpuid" : "=a"(max_ext) : "a"(0x80000000) : "ebx", "ecx", "edx");

    if (max_ext >= 0x80000004) {
        uint32_t* p = (uint32_t*)buffer;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            __asm__ volatile (
                "cpuid"
                : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "a"(leaf)
            );
            *p++ = eax;
            *p++ = ebx;
            *p++ = ecx;
            *p++ = edx;
        }
    } else {
        buffer[12] = '\0';
    }

    buffer[48] = '\0';
}
