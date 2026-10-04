#include "string.h"

// ============================================================
// ARCH-13: jalur cepat blok besar memakai instruksi string.
//
// SEBELUMNYA memcpy/memmove/memset adalah loop byte murni. Clang meng-unroll
// loop itu (4 byte per iterasi), yang tidak masalah untuk string pendek, tapi
// jalur panas menyalin blok besar:
//   * vmm_clone_user_as (fork)  — salin SELURUH address space
//   * elf_load_file             — salin setiap segmen ELF
//   * kwm_update_window (blit)  — salin seluruh canvas window
// Untuk ukuran-ukuran itu `rep movsb` jauh lebih cepat (satu instruksi vs
// puluhan ribu iterasi, dan pada CPU dengan ERMS/FSRM hardware menyalin
// dengan granularitas cache line).
//
// AMBANG 128 BYTE. Di bawah itu, `rep movsb` kalah: ia punya biaya setup
// ~30-40 cycle, sementara loop byte yang sudah di-unroll hanya butuh beberapa
// cycle per 4 byte. Ukuran kecil adalah mayoritas pemanggilan (strcpy,
// header, struct), jadi ambang ini penting — memakai rep untuk semuanya akan
// memperlambat jalur umum.
//
// ⚠ WAJIB `cld` SEBELUM INSTRUKSI STRING. Ini pelajaran langsung dari ARCH-1:
// DF dapat di-set dari ring 3 dan interrupt/trap gate TIDAK menormalkannya.
// Entry stub sekarang melakukan `cld` (KZ_ENTRY_PROLOGUE), tapi fungsi ini
// juga dipanggil dari konteks lain (boot, task kernel baru, AP) — dan
// ketergantungan pada "entry stub sudah cld" adalah asumsi tersembunyi yang
// persis menyebabkan bug ARCH-1. Karena itu `cld` ditulis eksplisit di sini,
// tepat sebelum instruksi yang membutuhkannya. Biayanya ~1 cycle.
// ============================================================

#define KZ_REP_THRESHOLD 128

/* Menyalin blok memori dari src ke dest (tidak aman jika overlap). */
void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* pdest = (uint8_t*)dest;
    const uint8_t* psrc = (const uint8_t*)src;

    if (n >= KZ_REP_THRESHOLD) {
        // cld + rep movsb: DF=0 dipaksa di sini, tidak diasumsikan.
        __asm__ volatile("cld; rep movsb"
                         : "+D"(pdest), "+S"(psrc), "+c"(n)
                         :
                         : "memory");
        return dest;
    }

    for (size_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }
    return dest;
}

/*
 * Menyalin blok memori dengan benar meski src dan dest overlap.
 * Jika dest > src dan region overlap, salin dari belakang ke depan
 * agar byte yang belum disalin tidak tertimpa duluan.
 */
void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) {
        return dest;
    }
    if (d < s || d >= s + n) {
        /* Tidak overlap, atau dest ada di sebelum src -- salin maju */
        if (n >= KZ_REP_THRESHOLD) {
            __asm__ volatile("cld; rep movsb"
                             : "+D"(d), "+S"(s), "+c"(n)
                             :
                             : "memory");
            return dest;
        }
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        /* Overlap dengan dest > src -- salin mundur untuk keamanan */
        if (n >= KZ_REP_THRESHOLD) {
            // std + rep movsb menyalin MUNDUR, lalu cld untuk memulihkan
            // keadaan normal (DF=0 adalah kontrak ABI; meninggalkannya 1 akan
            // merusak setiap instruksi string berikutnya di thread ini).
            // Pointer dimajukan ke elemen TERAKHIR dulu: dengan DF=1, rep
            // movsb men-decrement RSI/RDI, jadi ia menyalin dari sana mundur.
            uint8_t* dp = d + n - 1;
            const uint8_t* sp = s + n - 1;
            __asm__ volatile("std; rep movsb; cld"
                             : "+D"(dp), "+S"(sp), "+c"(n)
                             :
                             : "memory");
            return dest;
        }
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

/* Mengisi blok memori dengan nilai tertentu. */
void* memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;

    if (n >= KZ_REP_THRESHOLD) {
        // rep stosb mengisi AL ke [RDI] sebanyak RCX kali.
        // cld eksplisit — lihat catatan di atas memcpy.
        __asm__ volatile("cld; rep stosb"
                         : "+D"(p), "+c"(n)
                         : "a"((uint8_t)c)
                         : "memory");
        return s;
    }

    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

/* Membandingkan dua blok memori byte per byte. */
int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = (const uint8_t*)s1;
    const uint8_t* p2 = (const uint8_t*)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (int)p1[i] - (int)p2[i];
        }
    }
    return 0;
}

/* Menghitung panjang string (tidak termasuk null terminator). */
size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

/* Membandingkan dua string. Mengembalikan 0 jika identik. */
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/*
 * Membandingkan hingga n karakter pertama dari dua string.
 * Mengembalikan 0 jika n karakter pertama identik.
 */
int strncmp(const char* s1, const char* s2, size_t n) {
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/*
 * Menyalin hingga n karakter dari src ke dest.
 * Jika src lebih pendek dari n, sisa dest diisi '\0' (null-padding).
 * PERINGATAN: tidak menjamin null-terminator jika strlen(src) >= n.
 */
char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}