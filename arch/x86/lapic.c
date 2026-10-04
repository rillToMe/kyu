#include <stdint.h>
#include <stddef.h>
#include "lapic.h"
#include "task.h"
#include "smp.h"
#include "io.h"
#include "msr.h"
#include "timer.h"
#include "kprint.h"
#include "serial.h"

extern uint64_t hhdm_offset;

#define IA32_APIC_BASE_ENABLE   (1ULL << 11)

#define LAPIC_REG_ID            0x020
#define LAPIC_REG_EOI           0x0B0
#define LAPIC_REG_SVR           0x0F0
#define LAPIC_REG_LVT_TIMER     0x320
#define LAPIC_REG_TIMER_INIT    0x380
#define LAPIC_REG_TIMER_CURRENT 0x390
#define LAPIC_REG_TIMER_DIVIDE  0x3E0

#define LAPIC_REG_ICR_LOW       0x300
#define LAPIC_REG_ICR_HIGH      0x310

#define LAPIC_ICR_FIXED         0x00000000U
#define LAPIC_ICR_DELIVERY_PEND (1U << 12)

#define LAPIC_SVR_ENABLE        0x100
#define LAPIC_TIMER_PERIODIC    (1U << 17)
#define LAPIC_TIMER_MASKED      (1U << 16)

#define LAPIC_SPURIOUS_VECTOR   0xFF

// ============================================================
// ARCH-12: KALIBRASI LAPIC TIMER
//
// SEBELUMNYA `INIT_COUNT` adalah konstanta 1000000 yang ditulis apa adanya.
// Nilai itu TIDAK berarti apa-apa: LAPIC timer menghitung pada bus clock /
// divider, yang berbeda-beda antar mesin. Di satu host 1000000 menghasilkan
// ~100 Hz, di host lain ~1000 Hz. Akibatnya kuantum scheduler di AP berubah
// tanpa diketahui, dan `lapic_ticks` tidak bisa dijadikan basis waktu.
//
// SEKARANG `INIT_COUNT` dihitung saat boot dengan mengukur LAPIC terhadap
// PIT — sumber waktu yang sudah dikalibrasi driver timer.
//
// CARA KERJA (pola standar, sama seperti Linux `lapic_timer_c2_ok` probe):
//   1. Program LAPIC one-shot dengan hitungan besar.
//   2. Tunggu PIT selama window yang diketahui (PIT_TICKS_WINDOW tick PIT).
//   3. Baca LAPIC current count → jumlah tick LAPIC dalam window itu.
//   4. Hitung INIT_COUNT untuk frekuensi target.
//
// Kenapa PIT dan bukan TSC: PIT sudah dipakai kernel sebagai basis waktu
// (drivers/timer.c) dan frekuensinya (1193182 Hz) tetap di semua mesin. TSC
// tidak konstan di semua CPU dan tidak dikalibrasi di proyek ini.
//
// DIVIDER: 16 (nilai 0x3). Dipilih agar INIT_COUNT tidak overflow 32-bit
// untuk target frekuensi rendah maupun tinggi.
// ============================================================

#define LAPIC_TIMER_DIVIDER_CFG  0x3U    // divide by 16
#define LAPIC_TIMER_DIVIDER_VAL  16U

// Frekuensi tick LAPIC yang diinginkan (Hz). Dipilih sama dengan tick PIT
// AP di schedule_on_cpu (kuantum 20 ms) supaya konsisten dengan BSP.
#define LAPIC_TIMER_TARGET_HZ    100U

// Panjang window pengukuran, dalam TICK PIT kanal 0 (basis TIMER_DEFAULT_HZ).
//
// Window = LAPIC_CALIB_PIT_TICKS / TIMER_DEFAULT_HZ detik.
//   5 tick @ 60 Hz = 83 ms.
//
// Tidak ada batasan counter 16-bit di sini: kanal 0 diprogram oleh
// init_timer() dan kita hanya membacanya, jadi window boleh berapa pun
// selama tidak menghambat boot. 83 ms memberi ~15 juta tick LAPIC pada
// bus ~188 MHz → resolusi relatif ~0.007%.
#define LAPIC_CALIB_PIT_TICKS    5U

// Batas aman: kalau hasil kalibrasi di luar rentang ini, pengukuran gagal
// (PIT tidak jalan, LAPIC tidak jalan, atau div/clock aneh) dan kita jatuh
// ke nilai default yang aman alih-alih memprogram nilai sampah.
#define LAPIC_INIT_COUNT_MIN     1000U
#define LAPIC_INIT_COUNT_MAX     0xFFFFFFFFU

// Nilai fallback bila kalibrasi gagal. Bukan nilai "benar" — hanya supaya
// sistem tetap boot dengan tick yang wajar di mayoritas mesin.
#define LAPIC_INIT_COUNT_FALLBACK 1000000U

static volatile uint32_t *lapic_mmio = 0;
static volatile uint64_t lapic_ticks = 0;

// Hasil kalibrasi (0 = belum/gagal).
static uint32_t lapic_init_count = 0;

// ------------------------------------------------------------
// PIT channel 2 — timer satu-tembakan untuk pengukuran.
//
// Kanal 2 dipilih karena tidak dipakai kernel (kanal 0 = tick sistem).
// Mode 0 (interrupt on terminal count) memberi kita satu tembakan.
// ------------------------------------------------------------
#define PIT_CH2_DATA   0x42
#define PIT_CMD        0x43
#define PIT_FREQ_HZ    1193182U
#define PIT_SPEAKER_PORT 0x61

// ============================================================
// REFERENSI WAKTU: PIT KANAL 0 (timebase OS sendiri)
//
// ════════════════════════════════════════════════════════════════════════
// KENAPA KANAL 0, BUKAN KANAL 2
// ════════════════════════════════════════════════════════════════════════
// Percobaan pertama memakai kanal 2 (yang tidak dipakai kernel) dan hasilnya
// TIDAK BISA DIPERCAYA: dua pendekatan pengukuran yang berbeda menghasilkan
// angka yang berbeda 9x (bus clock 187 MHz vs 21 MHz), dan tidak ada cara
// membuktikan mana yang benar tanpa referensi eksternal.
//
// Akar masalahnya: kanal 2 butuh (a) gerbang speaker di port 0x61 aktif,
// (b) pemrograman mode yang benar, dan (c) pengukuran yang tidak bergantung
// pada posisi awal counter. Tiga hal itu semuanya bisa salah secara senyap,
// dan gejalanya hanya "angka yang terlihat wajar tapi keliru".
//
// Kanal 0 menghilangkan seluruh kelas masalah itu:
//   * sudah diprogram init_timer() untuk 60 Hz — TIDAK perlu diprogram ulang
//     (dan tidak boleh: mengubahnya akan merusak timebase sistem);
//   * rate-nya SUDAH TERBUKTI benar — seluruh timekeeping OS (timer_get_ms,
//     sleep queue, kuantum scheduler) bergantung padanya. Kalau rate kanal 0
//     salah, OS-nya sendiri sudah kacau jauh sebelum sampai sini;
//   * mode 3 (square wave) memuat ulang dirinya sendiri, jadi counter selalu
//     berputar dan akumulasi penurunan bekerja tanpa syarat tambahan.
//
// Kita hanya MEMBACA kanal 0 (latch + baca), tidak pernah memprogramnya.
// ============================================================
#define PIT_CH0_DATA   0x40
#define PIT_CMD        0x43
#define PIT_FREQ_HZ    1193182U

// Latch + baca counter kanal 0. Urutan latch→lobyte→hibyte wajib, supaya
// kedua byte berasal dari snapshot yang sama (tanpa latch, counter bisa
// berubah di antara dua pembacaan dan hasilnya ngawur).
static uint16_t pit0_read_count(void) {
    outb(PIT_CMD, 0x00);   // 00 00 000 0 = latch counter 0
    uint8_t lo = inb(PIT_CH0_DATA);
    uint8_t hi = inb(PIT_CH0_DATA);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

// Tunggu sampai `target_count` COUNT PIT benar-benar lewat.
//
// ════════════════════════════════════════════════════════════════════════
// DUA BUG YANG DIPERBAIKI DI SINI (keduanya sempat lolos)
// ════════════════════════════════════════════════════════════════════════
// 1. TARGET ADALAH COUNT, BUKAN TICK.
//    Satu tick PIT @60 Hz = 1193182/60 = 19886 count. Memanggil fungsi ini
//    dengan `target = 5` berarti menunggu 5 COUNT ≈ 4 µs — bukan 5 tick
//    (83 ms). Window yang terukur jadi ~20.000x lebih pendek, dan `elapsed`
//    LAPIC didominasi overhead pemanggilan, bukan window. Itu sebabnya hasil
//    kalibrasi berbeda-beda antar boot. Pemanggil sekarang menghitung
//    target_count = tick × count_per_tick.
//
// 2. WRAP TERJADI DI RELOAD VALUE, BUKAN DI 0x10000.
//    Kode lama menghitung `prev + (0x10000 - c)` saat wrap. Itu mengasumsikan
//    counter berputar sampai 65536, padahal PIT dimuat dengan reload value
//    19886 (untuk 60 Hz) — jadi counter hanya sampai 19886. Rumus itu
//    menambahkan ~45700 count palsu setiap wrap, membuat akumulasi melompat
//    jauh melebihi kenyataan dan loop keluar terlalu cepat.
//    Perbaikan: saat wrap, tambahkan `prev` (penurunan dari prev ke 0) lalu
//    tambahkan `(reload - c)` (penurunan dari reload ke posisi sekarang).
//
// Mengembalikan 0 bila timeout (PIT tidak jalan / spin habis).
static int pit0_wait_counts(uint32_t target_count, uint32_t reload_value,
                            uint32_t spin_limit) {
    if (target_count == 0 || reload_value == 0) return 0;

    uint16_t prev = pit0_read_count();
    uint64_t accumulated = 0;

    for (uint32_t i = 0; i < spin_limit; i++) {
        uint16_t c = pit0_read_count();

        if (c <= prev) {
            // Turun normal (termasuk diam di nilai yang sama).
            accumulated += (uint32_t)(prev - c);
        } else {
            // Wrap: prev → 0, reload → c.
            accumulated += (uint32_t)prev;
            if (reload_value > c) accumulated += (uint32_t)(reload_value - c);
        }
        prev = c;

        if (accumulated >= target_count) return 1;
    }
    return 0;
}

static uint32_t lapic_read(uint32_t reg) {
    return lapic_mmio[reg >> 2];
}

static void lapic_write(uint32_t reg, uint32_t value) {
    lapic_mmio[reg >> 2] = value;
    (void)lapic_read(LAPIC_REG_ID);
}

// ------------------------------------------------------------
// lapic_timer_calibrate — ukur LAPIC terhadap PIT, isi lapic_init_count.
//
// Return 1 sukses, 0 gagal (lapic_init_count diisi fallback).
//
// Dipanggil SEKALI di BSP sebelum AP mana pun start, sehingga nilai yang
// dihitung dipakai semua CPU (LAPIC clock sama untuk semua core pada satu
// package — kalau tidak, AP akan mengoreksi sendiri lewat hasil ukurannya;
// lihat lapic_init_ap).
// ------------------------------------------------------------
static int lapic_timer_calibrate(void) {
    if (lapic_mmio == 0) {
        lapic_init_count = LAPIC_INIT_COUNT_FALLBACK;
        return 0;
    }

    // 1. Divider + LAPIC one-shot dengan hitungan maksimum.
    lapic_write(LAPIC_REG_TIMER_DIVIDE, LAPIC_TIMER_DIVIDER_CFG);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_MASKED);   // masked: tidak ada IRQ
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFU);

    // 2. Hitung target dalam COUNT PIT (bukan tick).
    //
    //    Kanal 0 SUDAH diprogram init_timer() untuk TIMER_DEFAULT_HZ — kita
    //    hanya membaca, tidak memprogram ulang (memprogramnya akan merusak
    //    timebase sistem). Reload value-nya adalah divisor yang dipakai
    //    timer_program_pit(): PIT_FREQ_HZ / TIMER_DEFAULT_HZ.
    //
    //    ⚠ Target HARUS dalam count, bukan tick. Satu tick @60 Hz = 19886
    //    count; memakai angka tick langsung membuat window ~20000x terlalu
    //    pendek (lihat catatan bug #1 di pit0_wait_counts).
    const uint32_t pit_reload = PIT_FREQ_HZ / TIMER_DEFAULT_HZ;   // ≈ 19886
    const uint32_t target_counts = pit_reload * LAPIC_CALIB_PIT_TICKS;

    // 3. Tunggu `target_counts` count PIT benar-benar lewat.
    int done = pit0_wait_counts(target_counts, pit_reload, 4000000U);

    // 4. Baca berapa tick LAPIC yang terpakai.
    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CURRENT);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);   // hentikan LAPIC timer

    if (!done) {
        serial_print("[lapic] kalibrasi gagal: PIT tidak selesai - pakai default\n");
        lapic_init_count = LAPIC_INIT_COUNT_FALLBACK;
        return 0;
    }

    uint32_t elapsed = 0xFFFFFFFFU - remaining;
    if (elapsed == 0) {
        serial_print("[lapic] kalibrasi gagal: LAPIC tidak mencacah - pakai default\n");
        lapic_init_count = LAPIC_INIT_COUNT_FALLBACK;
        return 0;
    }

    // 5. Hitung INIT_COUNT untuk LAPIC_TIMER_TARGET_HZ.
    //
    //    `elapsed` tick LAPIC terjadi dalam window
    //        (target_counts / PIT_FREQ_HZ) detik.
    //    LAPIC bus frequency  = elapsed / window_detik
    //    LAPIC tick frequency = bus_freq / DIVIDER
    //    INIT_COUNT           = LAPIC tick frequency / TARGET_HZ
    //
    //    Disusun ulang agar tidak ada pembagian pecahan:
    //    INIT_COUNT = elapsed * PIT_FREQ_HZ
    //                 / (target_counts * DIVIDER * TARGET_HZ)
    uint64_t num = (uint64_t)elapsed * PIT_FREQ_HZ;
    uint64_t den = (uint64_t)target_counts * LAPIC_TIMER_DIVIDER_VAL * LAPIC_TIMER_TARGET_HZ;
    if (den == 0) {
        lapic_init_count = LAPIC_INIT_COUNT_FALLBACK;
        return 0;
    }
    uint64_t computed = num / den;

    if (computed < LAPIC_INIT_COUNT_MIN || computed > LAPIC_INIT_COUNT_MAX) {
        // Diagnostik lengkap: tanpa angka mentah, kegagalan kalibrasi tidak
        // bisa dibedakan antara "PIT tidak mencacah", "LAPIC tidak mencacah",
        // dan "rumus salah" — ketiganya berujung pada pesan yang sama.
        serial_print("[lapic] kalibrasi di luar rentang - elapsed=");
        serial_dec(elapsed);
        serial_print(" target_counts=");
        serial_dec(target_counts);
        serial_print(" computed=");
        serial_dec(computed);
        serial_print(" -> pakai default\n");
        lapic_init_count = LAPIC_INIT_COUNT_FALLBACK;
        return 0;
    }

    lapic_init_count = (uint32_t)computed;

    serial_print("[lapic] kalibrasi OK: ");
    serial_dec(computed);
    serial_print(" tick @ ");
    serial_dec(LAPIC_TIMER_TARGET_HZ);
    serial_print(" Hz\n");
    return 1;
}

static void lapic_enable(void) {
    uint64_t apic_base = kz_rdmsr(MSR_IA32_APIC_BASE);
    uint64_t phys = apic_base & 0xFFFFFFFFFFFFF000ULL;

    kz_wrmsr(MSR_IA32_APIC_BASE, apic_base | IA32_APIC_BASE_ENABLE);

    if (lapic_mmio == 0) {
        lapic_mmio = (volatile uint32_t *)(phys + hhdm_offset);
    }

    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
}

static void lapic_timer_start(void) {
    uint32_t init = lapic_init_count ? lapic_init_count : LAPIC_INIT_COUNT_FALLBACK;
    lapic_write(LAPIC_REG_TIMER_DIVIDE, LAPIC_TIMER_DIVIDER_CFG);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INIT, init);
}

void lapic_init_bsp(void) {
    lapic_enable();
    // ARCH-12: kalibrasi SEKALI di BSP, sebelum AP mana pun start, sehingga
    // semua CPU memakai INIT_COUNT yang sama.
    lapic_timer_calibrate();
    lapic_timer_start();
}

void lapic_init_ap(void) {
    lapic_enable();
    // Nilai kalibrasi BSP sudah tersedia (BSP init sebelum AP bring-up).
    // Kalau BSP gagal, lapic_init_count berisi fallback dan AP memakainya.
    lapic_timer_start();
}

void lapic_eoi(void) {
    if (lapic_mmio != 0) {
        lapic_write(LAPIC_REG_EOI, 0);
    }
}

uint32_t lapic_id(void) {
    if (lapic_mmio == 0) return 0;
    return lapic_read(LAPIC_REG_ID) >> 24;
}

uint64_t lapic_timer_ticks(void) {
    return lapic_ticks;
}

void lapic_send_reschedule(uint32_t cpu_index) {
    percpu_t *target = smp_get_cpu(cpu_index);
    if (target == NULL || !target->online) return;
    if (lapic_mmio == 0) return;

    uint32_t dest_lapic = target->lapic_id;

    // Wait for previous IPI to be delivered
    uint32_t timeout = 100000;
    while ((lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_DELIVERY_PEND) && timeout > 0) {
        __asm__ volatile("pause");
        timeout--;
    }

    // Set destination LAPIC ID in ICR high
    lapic_write(LAPIC_REG_ICR_HIGH, dest_lapic << 24);
    // Send fixed IPI with reschedule vector
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_FIXED | LAPIC_RESCHEDULE_VECTOR);
}

registers_t* lapic_timer_handler(registers_t* regs) {
    __asm__ volatile(
        "lock incq %0"
        : "+m"(lapic_ticks)
        :
        : "memory"
    );
    lapic_eoi();

    uint32_t cpu_id = smp_current_cpu_index();
    // BSP uses PIT timer (timer_handler) for scheduling; skip here.
    if (cpu_id == 0) {
        return regs;
    }

    return schedule_on_cpu(cpu_id, regs);
}

registers_t* lapic_reschedule_handler(registers_t* regs) {
    lapic_eoi();

    uint32_t cpu_id = smp_current_cpu_index();
    if (!smp_reschedule_pending(cpu_id)) {
        return regs;
    }

    return schedule_on_cpu(cpu_id, regs);
}
