// ============================================================
// kernel/sched/context_validate.c — Sanity check RSP hasil handler (ARCH-8)
//
// MASALAH YANG DITUTUP
//   Tiga ISR (timer, LAPIC timer, IPI reschedule) memakai pola:
//
//       call handler        ; return value = RSP task berikutnya
//       mov rsp, rax        ; <-- RSP dari C dipakai LANGSUNG
//       POPA64
//       iretq
//
//   Kalau handler mengembalikan nilai yang tidak wajar (bug di scheduler,
//   pointer rusak, races), `mov rsp, rax` memuat stack sampah dan kernel
//   crash di tempat yang jauh dari penyebabnya — biasanya sebagai #DF atau
//   triple fault, karena stack sudah tidak bisa dipakai untuk melaporkan apa
//   pun.
//
//   Linux memakai `WARN_ON` + `sync_regs()` pada jalur yang sama. Di sini
//   validasinya dilakukan sebelum RSP dipakai, dan kegagalannya dilaporkan
//   dengan pesan yang menyebut nilai RSP-nya — bukan crash misterius.
//
// APA YANG DICEK
//   1. Alamat canonical (bit 63..48 harus sama dengan bit 47).
//   2. Berada di HIGHER-HALF (bukan user range). Stack kernel selalu di
//      higher-half: stack boot dari Limine ada di HHDM (0xFFFF8000...), stack
//      task/idle/syscall dari kmalloc ada di zona heap (0xFFFF9000...).
//      RSP yang menunjuk ke user range berarti handler mengembalikan pointer
//      user — itu bug yang harus ditolak, karena `mov rsp, rax` akan membuat
//      kernel berjalan di atas stack milik aplikasi.
//   3. 8-byte aligned (registers_t adalah array qword).
//
// ════════════════════════════════════════════════════════════════════════
// TEMUAN PENTING: ADA DUA BENTUK FRAME, DAN RESIDUE ALIGNMENT-NYA BEDA
// ════════════════════════════════════════════════════════════════════════
// Ini ditemukan saat boot-test pertama: validasi versi awal menolak RSP yang
// sebenarnya SAH dan membanjiri serial dengan "[CTX] RSP tidak valid".
//
// CPU mendorong jumlah qword yang BERBEDA tergantung CPL saat interrupt:
//
//   Dari RING 3 (user) → 5 qword: SS, RSP, RFLAGS, CS, RIP
//                        (pergantian stack butuh SS:RSP)
//   Dari RING 0 (kernel) → 3 qword: RFLAGS, CS, RIP
//                        (stack tidak berganti; SS/RSP TIDAK didorong)
//
// Konsekuensinya, `registers_t` — yang selalu mendeklarasikan 5 field
// (rip, cs, rflags, rsp, ss) — untuk frame ring-0 membaca DUA field terakhir
// dari memori di ATAS frame (isi stack kode yang terinterupsi). Ini tidak
// berbahaya untuk dump/panic, dan TIDAK memengaruhi context switch karena
// `iretq` juga hanya mem-pop 3 qword saat kembali ke CPL yang sama — jadi
// jumlah yang didorong dan di-pop selalu konsisten.
//
// Tapi residue alignment-nya berbeda:
//   frame ring-3: RSP0 = stack_top (16-aligned) → registers_t ≡ 0 (mod 16)
//   frame ring-0: RSP  = RSP kode kernel, yang di dalam body fungsi ≡ 8
//                 (mod 16) karena `call` sudah mendorong return address
//                 → registers_t ≡ 8 (mod 16)
//
// Karena itu syarat "16-byte aligned" SALAH — ia menolak setiap interrupt
// yang datang saat kernel sedang berjalan. Yang benar adalah 8-byte aligned.
//
// CATATAN untuk ARCH-6 (alignment task baru): karena `iretq` ke CPL=0 tidak
// mem-pop SS/RSP, nilai RSP di slot frame task kernel DIABAIKAN — yang
// menentukan RSP awal task adalah posisi RSP setelah iretq. Dengan padding
// 8 byte, posisi itu = stack_top - 24 ≡ 8 (mod 16), yaitu keadaan yang benar
// untuk masuk sebuah fungsi (setara "baru saja di-`call`"). Untuk task
// ring-3, slot RSP/SS memang dipakai (pergantian stack), jadi nilainya sah.
// ============================================================

#include <stdint.h>
#include "task.h"
#include "smp.h"
#include "serial.h"
#include "kprint.h"

// Batas atas ruang alamat canonical 48-bit (awal higher-half).
#define KZ_CANONICAL_MAX 0x0000800000000000ULL
#define KZ_CANONICAL_MIN 0xFFFF800000000000ULL

static int kz_is_canonical(uint64_t addr) {
    return (addr < KZ_CANONICAL_MAX) || (addr >= KZ_CANONICAL_MIN);
}

// Return 1 bila `rsp` layak dipakai sebagai pointer ke registers_t.
int context_rsp_is_valid(uint64_t rsp) {
    if (!kz_is_canonical(rsp)) return 0;
    // Stack kernel WAJIB higher-half. Alamat user di sini berarti handler
    // mengembalikan pointer user — menolaknya mencegah kernel berjalan di
    // atas stack milik aplikasi (yang bisa dikendalikan dari ring 3).
    if (rsp < KZ_CANONICAL_MIN) return 0;
    // 8-byte (bukan 16-byte) aligned — lihat penjelasan dua bentuk frame
    // di atas: residue 0 dan 8 keduanya SAH.
    if (rsp & 0x7ULL) return 0;
    // registers_t butuh 176 byte; pastikan tidak membentur ujung ruang alamat.
    if (rsp > ~0ULL - 256ULL) return 0;
    return 1;
}

// Dipanggil dari stub saat validasi GAGAL. Mencetak konteks secukupnya lalu
// mengembalikan RSP lama (frame yang sedang dipakai ISR) supaya kernel bisa
// melanjutkan dan melaporkan lewat jalur normal — jauh lebih berguna daripada
// memuat RSP sampah.
//
// `bad_rsp` = nilai yang ditolak; `fallback_rsp` = RSP frame ISR saat ini
// (sudah valid, karena ISR baru saja berjalan di atasnya).
uint64_t context_rsp_fallback(uint64_t bad_rsp, uint64_t fallback_rsp,
                              uint64_t vector) {
    static volatile uint32_t report_count = 0;

    if (report_count < 8) {
        report_count++;
        serial_print("\n[CTX] RSP tidak valid dari handler: ");
        serial_print_hex(bad_rsp);
        serial_print(" vektor=");
        serial_dec(vector);
        serial_print(" cpu=");
        serial_dec((uint64_t)smp_current_cpu_index());
        serial_print(" -> pakai RSP lama (task ini lanjut)\n");
    }
    return fallback_rsp;
}
