#ifndef SMAP_H
#define SMAP_H

// FIX_005 Tahap 4 — SMEP/SMAP.
//
// Setelah CR4.SMAP aktif, kernel TIDAK bisa baca/tulis halaman US=1 kecuali
// EFLAGS.AC di-set (stac). Semua deref user yang disengaja dibungkus
// user_access_begin()/user_access_end() — jendela dibuat sesempit mungkin.
//
// stac/clac di-encode manual (.byte) supaya tidak tergantung feature-gating
// assembler; eksekusi dijaga g_smap_enabled (CPU tanpa SMAP → #UD).

// 1 jika CR4.SMAP berhasil diaktifkan (diisi cpu_enable_smap_smep).
extern int g_smap_enabled;

// ARCH-5: 1 jika EFER.NXE berhasil diaktifkan (diisi cpu_enable_nx).
// Hanya bila 1, bit NX (63) boleh diset pada PTE.
extern int g_nx_enabled;

// Aktifkan EFER.NXE. Per-CPU — panggil di BSP (kernel_main) DAN setiap AP.
// Return tidak ada; hasil dicek lewat g_nx_enabled.
void cpu_enable_nx(void);

static inline void user_access_begin(void) {
    if (g_smap_enabled)
        __asm__ volatile(".byte 0x0F, 0x01, 0xCB" ::: "memory", "cc"); // stac
}

static inline void user_access_end(void) {
    if (g_smap_enabled)
        __asm__ volatile(".byte 0x0F, 0x01, 0xCA" ::: "memory", "cc"); // clac
}

// Per-CPU: cek CPUID leaf 7 lalu set CR4.SMEP (bit 20) / CR4.SMAP (bit 21)
// sesuai dukungan. Panggil di BSP (kernel_main) DAN setiap AP (smp_ap_main)
// — CR4 bersifat per-core.
void cpu_enable_smap_smep(void);

// Pastikan CR0.WP aktif (kernel tidak boleh menembus halaman read-only).
// Per-CPU juga. Return 1 jika WP sudah/berhasil di-set.
int cpu_verify_wp(void);

#endif
