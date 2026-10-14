// ============================================================
// kernel/sched/fpu.c — Save/restore state FPU/SSE per task (ARCH-10)
//
// ════════════════════════════════════════════════════════════════════════
// MASALAH YANG DITUTUP
// ════════════════════════════════════════════════════════════════════════
// Sebelum ini TIDAK ADA penyimpanan state FPU/SSE antar context switch.
// Register XMM dan stack x87 adalah state GLOBAL per-CPU: task yang sedang
// berjalan menulis ke sana, dan task berikutnya melihat sisa nilai
// pendahulunya.
//
// Dua konsekuensi nyata:
//
//   1. KEBOCORAN DATA ANTAR PROSES. Task A menghitung dengan SSE, lalu
//      di-preempt. Task B membaca XMM yang masih berisi hasil Task A. Itu
//      pelanggaran isolasi yang sama seriusnya dengan membaca memori proses
//      lain — dan tidak terdeteksi oleh page table.
//
//   2. HASIL KOMPUTASI SALAH SECARA ACAK. Dua task yang sama-sama memakai
//      SSE akan saling merusak hasil, dan bug-nya muncul/hilang tergantung
//      urutan penjadwalan. Ini kelas bug yang paling mahal didiagnosis.
//
// ════════════════════════════════════════════════════════════════════════
// MENGAPA INI ADA
// ════════════════════════════════════════════════════════════════════════
// State FPU/SSE adalah state GLOBAL per-CPU, jadi ia harus disimpan dan
// dipulihkan setiap kali task berganti. Dua jalur userspace memang memakai
// SSE: target Rust `x86_64-unknown-none` mengaktifkan sse/sse2/fxsr secara
// default, dan `apps/calc.c` memakai float. Sejak Phase 3 seluruh userspace
// boleh memakai float/double (flag -mno-sse dilepas dari build userspace).
//
// KERNEL SENDIRI TETAP -mno-sse, dan itu BUKAN sisa kebijakan lama:
//
//   Interrupt entry TIDAK menyimpan XMM. arch/x86/isr_macro.inc hanya
//   mendorong register umum (PUSHA64), dan fpu_switch() di bawah hanya
//   berjalan saat CONTEXT SWITCH. Kalau kode kernel mengemisikan SSE, sebuah
//   interrupt yang datang saat task ring-3 memakai XMM akan menjalankan kode
//   kernel yang menimpa register itu, dan task tersebut lanjut dengan nilai FP
//   yang salah — diam-diam, tergantung di mana timer menyala.
//
//   Itu bukan hipotesis: kernel/string.c tanpa -mno-sse menghasilkan
//   `movups %xmm0` di memcpy, dan memcpy bisa dipanggil dari jalur scheduler
//   dan filesystem yang berjalan DI DALAM interrupt.
//
//   Karena itu kernel tetap dilarang FP (KYUZEN_KERNEL_FP_BAN di
//   cmake/KyuzenCompiler.cmake) dan dijaga setelah link
//   (cmake/verify_kernel_elf.cmake). Userspace adalah kasus yang BERBEDA:
//   fpu_switch() memang berjalan di antara task, jadi user code boleh
//   memakai FP. Asimetri itulah keseluruhan desainnya.
//
// ════════════════════════════════════════════════════════════════════════
// KENAPA FXSAVE/FXRSTOR, BUKAN XSAVE
// ════════════════════════════════════════════════════════════════════════
// FXSAVE menyimpan x87 + XMM0..XMM15 + MXCSR dalam 512 byte dengan alamat
// yang hanya perlu 16-byte aligned. Itu mencakup SEMUA register yang bisa
// disentuh SSE/SSE2 — dan SSE2 adalah batas maksimum yang dijanjikan kernel
// ke userspace.
//
// PENTING: FXSAVE TIDAK menyimpan bagian atas YMM/ZMM. Itu urusan XSAVE, yang
// belum dipakai di sini. Konsekuensinya: ELF userspace yang memuat instruksi
// AVX akan kehilangan 128 bit atasnya saat task berganti. Karena itu
// cmake/verify_user_elf.cmake MENOLAK ymm/zmm pada ELF userspace, sementara
// xmm dan x87 diizinkan. Kalau AVX mau didukung, modul ini titik masuknya —
// dan XSAVE harus menggantikan FXSAVE lebih dulu.
//
// XSAVE/XSAVEOPT lebih cepat (bisa melewati komponen yang tidak berubah),
// tapi membutuhkan: CPUID probe, ukuran area dinamis (XSAVE area bisa ribuan
// byte bila AVX-512 aktif), alokasi per task, dan penanganan XCR0. Untuk
// cakupan SSE/SSE2 saja, biaya kompleksitas itu tidak sebanding.
//
// ════════════════════════════════════════════════════════════════════════
// BIAYA
// ════════════════════════════════════════════════════════════════════════
//   * Ruang: 512 byte × MAX_TASKS(16) = 8 KB BSS di TCB.
//   * Waktu: ~100-200 cycle per context switch (FXSAVE + FXRSTOR), terhadap
//     kuantum 10-20 ms. Tidak terukur.
//
// ════════════════════════════════════════════════════════════════════════
// CR0 dan CR4: KEDUANYA WAJIB
// ════════════════════════════════════════════════════════════════════════
// CR0.EM harus 0. Bila EM=1 (default sebagian firmware), SETIAP instruksi
// FP/SSE menghasilkan #NM, x87 maupun SSE — jadi SSE fault walaupun CR4 sudah
// benar. CR0.MP=1 menyatakan FPU benar-benar ada, dan CR0.NE=1 melaporkan
// error x87 lewat #MF alih-alih PIC.
//
// CR4.OSFXSR=1 wajib: tanpa itu, setiap instruksi SSE dari ring 3
// menghasilkan #UD. Jadi fpu_init_cpu() bukan sekadar "optimasi" — ia
// prasyarat agar app yang memakai SSE bisa berjalan sama sekali.
// OSXMMEXCPT juga diset supaya exception SIMD dilaporkan lewat #XM (vektor 19)
// alih-alih #UD.
//
// Catatan: CR4.TSD (Time Stamp Disable) dan CR0.TS TIDAK dipakai. Teknik
// "lazy FPU" (set CR0.TS, tangani #NM, baru save/restore saat benar-benar
// dipakai) lebih cepat untuk beban kerja yang jarang memakai FPU, tapi
// membutuhkan handler #NM yang benar dan membuka jalur fault tambahan di
// context switch. Untuk OS ini, save/restore eager jauh lebih sederhana dan
// biayanya tidak terukur pada kuantum 10-20 ms.
// ============================================================

#include <stdint.h>
#include <stddef.h>
#include "task.h"
#include "smp.h"
#include "serial.h"

#define CR0_MP  (1ULL << 1)     // Monitor coProcessor: FPU waits for #WAIT/FWAIT
#define CR0_EM  (1ULL << 2)     // Emulation: forces #NM on every FP instruction
#define CR0_NE  (1ULL << 5)     // Numeric Error: report x87 errors via #MF, not PIC
#define CR0_TS  (1ULL << 3)     // Task Switched: lazy-FPU trigger (deliberately unused)

#define CR4_OSFXSR     (1ULL << 9)    // OS support for FXSAVE/FXRSTOR
#define CR4_OSXMMEXCPT (1ULL << 10)   // OS support for unmasked SIMD exceptions

// ------------------------------------------------------------
// fpu_init_cpu — aktifkan FPU/SSE di CPU ini.
//
// Per-CPU: CR0 dan CR4 bersifat per-core, jadi BSP dan setiap AP wajib
// memanggilnya.
//
// CR0:
//   EM=0  WAJIB. Bila EM=1, SETIAP instruksi FP/SSE menghasilkan #NM
//         ("device not available") — x87 maupun SSE. Ini kondisi default
//         sebagian firmware, dan tanpanya kode yang memakai FP akan fault
//         walaupun CR4 sudah benar.
//   MP=1  FPU hadir dan tidak diemulasi; FWAIT menunggu hasil, bukan
//         mengabaikannya.
//   NE=1  Error x87 dilaporkan lewat #MF (vektor 16) alih-alih PIC. Tanpa
//         ini, error numerik tidak bisa ditangani dengan benar.
//   TS=0  SENGAJA tidak dipakai. Teknik "lazy FPU" (set TS, tangani #NM,
//         baru save/restore saat benar-benar dipakai) lebih cepat untuk beban
//         kerja yang jarang memakai FPU, tapi membutuhkan handler #NM yang
//         benar dan membuka jalur fault tambahan di context switch. Save/restore
//         eager jauh lebih sederhana dan biayanya tidak terukur pada kuantum
//         10-20 ms.
//
// CR4:
//   OSFXSR=1     WAJIB. Tanpa ini, setiap instruksi SSE dari ring 3
//                menghasilkan #UD.
//   OSXMMEXCPT=1 Exception SIMD dilaporkan lewat #XM (vektor 19) alih-alih
//                #UD.
// ------------------------------------------------------------
void fpu_init_cpu(void) {
    uint32_t eax, ebx, ecx, edx;

    // CPUID leaf 1: EDX bit 24 = FXSR, bit 25 = SSE, bit 26 = SSE2.
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));

    // Kalau CPU tidak punya FXSR, FXSAVE tidak ada dan kita tidak bisa
    // menyimpan state. Tidak ada fallback yang berguna: jangan sentuh CR0/CR4,
    // dan biarkan userspace SSE fault. Ini tidak akan terjadi pada x86-64
    // nyata (SSE2 selalu ada di baseline), jadi cabang ini hanya jaring
    // pengaman.
    if (!(edx & (1u << 24))) return;

    // --- CR0: matikan emulasi, aktifkan FPU asli ---
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));

    cr0 &= ~(CR0_EM | CR0_TS);   // EM=0 (FP/SSE dijalankan, bukan diemulasi)
                                 // TS=0 (tidak ada lazy switching)
    cr0 |= CR0_MP | CR0_NE;      // MP=1 (FPU ada), NE=1 (error lewat #MF)

    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    // --- CR4: dukungan FXSAVE/FXRSTOR + pelaporan exception SIMD ---
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    cr4 |= CR4_OSFXSR;
    if (edx & (1u << 25)) cr4 |= CR4_OSXMMEXCPT;   // SSE tersedia

    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    // Inisialisasi keadaan FPU CPU ini ke kondisi bersih. Tanpa ini, state
    // awal bisa berupa sisa dari bootloader (Limine memakai SSE) dan task
    // pertama akan mewarisinya.
    __asm__ volatile("fninit" ::: "memory");
}

// ------------------------------------------------------------
// fpu_state_reset — tandai state task sebagai "belum pernah disimpan".
//
// Saat restore pertama, fpu_switch() akan memuat keadaan bersih (FNINIT)
// alih-alih memulihkan 512 byte sampah dari BSS.
// ------------------------------------------------------------
void fpu_state_reset(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;
    tasks[task_id].fpu_initialized = 0;
}

// ------------------------------------------------------------
// fpu_switch — simpan state task lama, muat state task baru.
//
// Dipanggil dari schedule_on_cpu() tepat setelah tugas pemilihan selesai,
// SEBELUM frame task baru dipakai. Pada titik itu `prev` masih berjalan di
// CPU ini (state FPU-nya masih hidup di register) dan `next` belum mulai.
//
// Urutan save-lalu-restore penting: FXSAVE menulis register yang sedang
// hidup ke memori; kalau FXRSTOR dijalankan lebih dulu, state `prev` sudah
// hilang dan tidak bisa disimpan lagi.
// ------------------------------------------------------------
void fpu_switch(int prev_id, int next_id) {
    // 1. Simpan state task yang meninggalkan CPU.
    if (prev_id >= 0 && prev_id < MAX_TASKS) {
        // Alamat area 512 byte di TCB, 16-byte aligned (dijamin atribut
        // aligned(16) pada field; FXSAVE #GP bila tidak aligned).
        void* area = (void*)tasks[prev_id].fpu_state;
        __asm__ volatile("fxsave64 (%0)" :: "r"(area) : "memory");
        tasks[prev_id].fpu_initialized = 1;
    }

    // 2. Muat state task yang akan berjalan.
    if (next_id >= 0 && next_id < MAX_TASKS) {
        if (tasks[next_id].fpu_initialized) {
            const void* area = (const void*)tasks[next_id].fpu_state;
            __asm__ volatile("fxrstor64 (%0)" :: "r"(area) : "memory");
        } else {
            // Task baru: belum pernah menyimpan state. Mulai dari keadaan
            // bersih supaya ia tidak mewarisi sisa register task sebelumnya
            // (itulah kebocoran yang ditutup modul ini).
            __asm__ volatile("fninit" ::: "memory");
            // fninit hanya mereset x87. XMM tidak punya instruksi "reset",
            // jadi kita bangun keadaan bersih lewat FXRSTOR dari area yang
            // sudah di-zero + MXCSR default. Area fpu_state di-zero saat TCB
            // dibuat (BSS), jadi cukup set MXCSR ke nilai default 0x1F80
            // (semua exception masked, round-to-nearest) langsung di area
            // itu lalu restore.
            //
            // Tanpa langkah ini, XMM task baru berisi apa pun yang tersisa di
            // register — persis kebocoran yang ingin dihilangkan.
            uint8_t* area = tasks[next_id].fpu_state;
            // MXCSR ada di offset 24 pada layout FXSAVE.
            *(uint32_t*)(area + 24) = 0x00001F80u;
            // FCW (x87 control word) default juga, di offset 0.
            *(uint16_t*)(area + 0) = 0x037Fu;
            __asm__ volatile("fxrstor64 (%0)" :: "r"(area) : "memory");
            tasks[next_id].fpu_initialized = 1;
        }
    } else {
        // Idle / tidak ada task: bersihkan FPU supaya tidak ada state task
        // yang tertinggal hidup di register saat CPU menganggur.
        __asm__ volatile("fninit" ::: "memory");
    }
}
