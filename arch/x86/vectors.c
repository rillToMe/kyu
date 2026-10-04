// ============================================================
// arch/x86/vectors.c — Handler default untuk vektor tanpa driver (ARCH-3)
//
// SEBELUMNYA: hanya 20 dari 256 vektor yang punya IDT gate. Setiap vektor
// lain (mis. 18 #MC, 16 #MF, 19 #XM, 255 spurious LAPIC yang MEMANG disetel
// di SVR, dan setiap IRQ PIC yang tidak dimask) jatuh ke IDT entry nol →
// #GP → #DF → triple fault → reset tanpa BSOD, tanpa crashdump, tanpa log.
//
// SEKARANG: semua 256 vektor terpasang. Yang tidak punya driver/handler
// khusus mendarat di sini dan diproses sesuai kelasnya:
//
//   1. SPURIOUS LAPIC (0xFF) — bukan error. LAPIC mengirim vektor ini saat
//      EOI race; cukup EOI dan kembali. Linux: asm_sysvec_spurious_apic_interrupt.
//
//   2. PIC IRQ (32..47) tanpa driver — EOI ke PIC (dan slave bila perlu),
//      lalu kembali. Ini yang mencegah IRQ tak terduga membunuh sistem.
//      Dibatasi MAX_REPORTS kali agar tidak membanjiri serial saat IRQ
//      storm (mis. device yang terus menandai interrupt).
//
//   3. Vektor exception (0..31) yang tidak punya handler khusus — ini
//      FAULT NYATA (#MC, #MF, #XM, #VE, #CP, ...). Tidak ada pemulihan yang
//      aman: laporkan dengan jelas lalu masuk jalur panic yang sudah ada.
//
//   4. Sisa vektor (48..255 non-spurious) — software interrupt atau IPI
//      tak dikenal. Tidak ada yang bisa di-EOI; laporkan lalu panic.
//
// CATATAN KEAMANAN: handler ini TIDAK PERNAH "melanjutkan seolah tidak
// terjadi apa-apa" untuk vektor exception. Hanya kelas IRQ/spurious yang
// boleh iretq tanpa panic — karena keduanya memang bukan kondisi error.
// ============================================================

#include <stdint.h>
#include "task.h"      // registers_t
#include "arch.h"      // KZ_VEC_*
#include "serial.h"
#include "kprint.h"
#include "io.h"
#include "lapic.h"

// Handler panic yang sudah ada (kernel/panic/panic.c). Noreturn pada
// praktiknya: ia menahan sistem di loop interaktif, jadi kita tidak
// mengandalkan nilai kembaliannya.
extern void exception_handler(registers_t* r);

// PIC ports — lihat drivers/keyboard.c pic_remap().
#define PIC1_CMD 0x20
#define PIC2_CMD 0xA0

#define MAX_REPORTS 8

// Hitung laporan per kelas supaya IRQ storm tidak membanjiri serial.
static volatile uint32_t irq_report_count = 0;
static volatile uint32_t unknown_report_count = 0;

// EOI ke 8259 PIC. IRQ 8..15 datang lewat slave, jadi butuh EOI ke dua-duanya
// (slave dulu, lalu master) — simetris dengan mouse_handler().
static void pic_eoi(uint64_t vector) {
    if (vector >= 40) {
        outb(PIC2_CMD, 0x20);   // IRQ 8..15 → slave
    }
    outb(PIC1_CMD, 0x20);       // selalu master
}

void default_interrupt_handler(registers_t* r) {
    if (!r) return;

    uint64_t v = r->int_num;

    // --- 1. Spurious LAPIC (0xFF) -------------------------------------
    // Bukan error: LAPIC memakai vektor ini untuk interrupt yang sudah
    // selesai sebelum dilayani. Cukup EOI dan lanjut.
    if (v == KZ_VEC_LAPIC_SPURIOUS) {
        lapic_eoi();
        return;
    }

    // --- 2. PIC IRQ tanpa driver (32..47) -----------------------------
    // EOI supaya PIC tidak memblokir IRQ berikutnya, lalu lanjut. Tanpa
    // EOI, satu IRQ tak dikenal akan membekukan seluruh level IRQ itu.
    if (v >= KZ_VEC_IRQ0 && v < 48) {
        pic_eoi(v);
        if (irq_report_count < MAX_REPORTS) {
            irq_report_count++;
            serial_print("[IRQ] tak ada driver untuk vektor ");
            serial_dec(v);
            serial_print(" (IRQ ");
            serial_dec(v - KZ_VEC_IRQ0);
            serial_print(") - di-EOI\n");
        }
        return;
    }

    // --- 3. Exception tanpa handler khusus (0..31) --------------------
    // #MC, #MF, #XM, #VE, #CP, #VC, #SX, #AC, dan reserved. Tidak ada
    // pemulihan yang aman — serahkan ke jalur panic standar supaya BSOD +
    // crashdump tetap dihasilkan (bukan triple fault).
    if (v < KZ_VEC_IRQ0) {
        serial_print("\n[EXC] vektor exception tanpa handler khusus: ");
        serial_dec(v);
        serial_print(" err=");
        serial_print_hex(r->error_code);
        serial_print(" rip=");
        serial_print_hex(r->rip);
        serial_print("\n");
        exception_handler(r);
        return;
    }

    // --- 4. Sisa vektor (48..255, bukan spurious) ---------------------
    // Software interrupt atau IPI tak dikenal. Tidak ada EOI yang masuk akal
    // (bukan IRQ controller), jadi laporkan lalu panic.
    if (unknown_report_count < MAX_REPORTS) {
        unknown_report_count++;
        serial_print("\n[VEC] vektor tak dikenal: ");
        serial_dec(v);
        serial_print(" rip=");
        serial_print_hex(r->rip);
        serial_print(" cs=");
        serial_print_hex(r->cs);
        serial_print("\n");
    }
    exception_handler(r);
}
