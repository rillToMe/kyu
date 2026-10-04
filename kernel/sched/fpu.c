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
// MENGAPA INI BARU PERLU SEKARANG
// ════════════════════════════════════════════════════════════════════════
// Kernel dibangun dengan `-mno-sse -mno-sse2 -msoft-float`, jadi kode kernel
// tidak pernah menyentuh SSE. Asumsi lama: "tidak ada state untuk disimpan."
// Asumsi itu BENAR untuk kernel, tapi SALAH untuk userspace:
//
//   * target Rust `x86_64-unknown-none` mengaktifkan sse/sse2/fxsr secara
//     default (lihat rust/target/.rustc_info.json), sehingga app Rust
//     menghasilkan instruksi SSE;
//   * `apps/calc.c` juga memakai float.
//
// Jadi ada dua jalur userspace yang menjalankan SSE sementara kernel tidak
// menyiapkan state-nya.
//
// ════════════════════════════════════════════════════════════════════════
// KENAPA FXSAVE/FXRSTOR, BUKAN XSAVE
// ════════════════════════════════════════════════════════════════════════
// FXSAVE menyimpan x87 + XMM0..XMM15 + MXCSR dalam 512 byte dengan alamat
// yang hanya perlu 16-byte aligned. Itu mencakup SEMUA register yang bisa
// disentuh SSE/SSE2 — dan SSE2 adalah batas maksimum yang dijanjikan kernel
// ke userspace.
//
// XSAVE/XSAVEOPT lebih cepat (bisa melewati komponen yang tidak berubah),
// tapi membutuhkan: CPUID probe, ukuran area dinamis (XSAVE area bisa ribuan
// byte bila AVX-512 aktif), alokasi per task, dan penanganan XCR0. Untuk
// cakupan SSE/SSE2 saja, biaya kompleksitas itu tidak sebanding. Bila AVX
// ditambahkan nanti, modul ini titik masuk yang tepat untuk beralih.
//
// ════════════════════════════════════════════════════════════════════════
// BIAYA
// ════════════════════════════════════════════════════════════════════════
//   * Ruang: 512 byte × MAX_TASKS(16) = 8 KB BSS di TCB.
//   * Waktu: ~100-200 cycle per context switch (FXSAVE + FXRSTOR), terhadap
//     kuantum 10-20 ms. Tidak terukur.
//
// ════════════════════════════════════════════════════════════════════════
// CR4.OSFXSR WAJIB
// ════════════════════════════════════════════════════════════════════════
// Tanpa CR4.OSFXSR=1, setiap instruksi SSE dari ring 3 menghasilkan #UD.
// Jadi fpu_init_cpu() bukan sekadar "optimasi" — ia prasyarat agar app yang
// memakai SSE bisa berjalan sama sekali. OSXMMEXCPT juga diset supaya
// exception SIMD dilaporkan lewat #XM (vektor 19) alih-alih #UD.
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

#define CR4_OSFXSR     (1ULL << 9)    // OS support for FXSAVE/FXRSTOR
#define CR4_OSXMMEXCPT (1ULL << 10)   // OS support for unmasked SIMD exceptions

// ------------------------------------------------------------
// fpu_init_cpu — aktifkan dukungan FXSAVE/FXRSTOR di CPU ini.
//
// Per-CPU: CR4 bersifat per-core, jadi BSP dan setiap AP wajib memanggilnya.
// ------------------------------------------------------------
void fpu_init_cpu(void) {
    uint32_t eax, ebx, ecx, edx;

    // CPUID leaf 1: EDX bit 24 = FXSR, bit 25 = SSE, bit 26 = SSE2.
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));

    // Kalau CPU tidak punya FXSR, FXSAVE tidak ada dan kita tidak bisa
    // menyimpan state. Tidak ada fallback yang berguna: jangan set bit CR4,
    // dan biarkan userspace SSE fault (yang memang tidak akan pernah
    // dihasilkan karena toolchain menargetkan x86-64 yang selalu punya SSE2).
    if (!(edx & (1u << 24))) return;

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
