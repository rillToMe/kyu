// ============================================================
// KERNEL ENTROPY — kernel/entropy.c
// RDRAND (Intel DRNG) untuk syscall 87. Tanpa state, tanpa lock
// (RDRAND instruksi-level), tanpa heap. Gagal jujur bila CPU tak
// mendukung — tidak ada fallback LCG/PIT (haram untuk seed TLS).
// ============================================================
#include <stdint.h>
#include "entropy.h"

static int g_detected = 0;
static int g_rdrand_ok = 0;

int entropy_has_rdrand(void) {
    if (!g_detected) {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1));
        g_rdrand_ok = (int)((ecx >> 30) & 1u);
        g_detected = 1;
    }
    return g_rdrand_ok;
}

// Satu word 64-bit. Retry ≤10x sesuai Intel Manual (§7.3.17.2:
// kegagalan sesekali normal di beban berat, bukan sinyal rusak).
static int rdrand64(uint64_t *v) {
    unsigned char ok = 0;
    uint64_t val = 0;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
        if (ok) {
            *v = val;
            return 1;
        }
    }
    return 0;
}

int entropy_fill(uint8_t *out, uint32_t len) {
    uint32_t off = 0;

    if (out == 0 || len == 0 || len > ENTROPY_MAX)
        return ENTROPY_ERR;
    if (!entropy_has_rdrand())
        return ENTROPY_ENOHW;
    while (off + 8 <= len) {
        uint64_t v;
        int i;
        if (!rdrand64(&v))
            return ENTROPY_ERR;
        for (i = 0; i < 8; i++)
            out[off++] = (uint8_t)(v >> (i * 8));
    }
    if (off < len) {
        uint64_t v;
        if (!rdrand64(&v))
            return ENTROPY_ERR;
        while (off < len) {
            out[off++] = (uint8_t)v;
            v >>= 8;
        }
    }
    return (int)len;
}
