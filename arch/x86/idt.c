#include <stdint.h>
#include "string.h"
#include "arch.h"

// ------------------------------------------------------------
// Stub yang punya file sendiri (butuh penanganan khusus).
// ------------------------------------------------------------
extern void keyboard_isr_stub();
extern void timer_isr_stub();
extern void lapic_timer_isr_stub();
extern void reschedule_isr_stub();
extern void isr14_stub();       // #PF — butuh CR2 sebagai arg2
extern void isr128_stub();      // syscall int 0x80
extern void mouse_isr_stub();

// ------------------------------------------------------------
// Stub exception dari exceptions.asm.
//
// ARCH-3: semua vektor 0..31 kini punya stub. Yang butuh penanganan khusus
// (CR2 sebagai arg2, dst) tetap dirujuk eksplisit; sisanya cukup lewat
// isr_default_table untuk vektor 32..255.
//
// Semua simbol isrN_stub untuk N di 0..31 dideklarasikan di bawah — jangan
// hapus yang tampak "tidak dipakai": setiap vektor WAJIB terpasang, dan
// mendeklarasikannya di sini membuat vektor yang hilang jadi error kompilasi
// (link error) alih-alih IDT entry nol yang memicu #GP → #DF.
// ------------------------------------------------------------
extern void isr0_stub();    // #DE Divide Error
extern void isr1_stub();    // #DB Debug
extern void isr2_stub();    // NMI
extern void isr3_stub();    // #BP Breakpoint
extern void isr4_stub();    // #OF Overflow
extern void isr5_stub();    // #BR BOUND Range
extern void isr6_stub();    // #UD Invalid Opcode
extern void isr7_stub();    // #NM Device Not Available
extern void isr8_stub();    // #DF Double Fault
extern void isr9_stub();    // (legacy, tidak dipakai di x86-64)
extern void isr10_stub();   // #TS Invalid TSS
extern void isr11_stub();   // #NP Segment Not Present
extern void isr12_stub();   // #SS Stack-Segment Fault
extern void isr13_stub();   // #GP General Protection
extern void isr15_stub();   // reserved
extern void isr16_stub();   // reserved
extern void isr17_stub();   // #AC Alignment Check
extern void isr18_stub();   // #MC Machine Check
extern void isr19_stub();   // #XM SIMD Floating-Point
extern void isr20_stub();   // #VE Virtualization Exception
extern void isr21_stub();   // #CP Control Protection
extern void isr22_stub();   // reserved
extern void isr23_stub();   // reserved
extern void isr24_stub();   // reserved
extern void isr25_stub();   // reserved
extern void isr26_stub();   // reserved
extern void isr27_stub();   // reserved
extern void isr28_stub();   // reserved
extern void isr29_stub();   // #VC VMM Communication
extern void isr30_stub();   // #SX Security Exception
extern void isr31_stub();   // reserved

// Tabel alamat stub default vektor 32..255 (di-emit oleh exceptions.asm).
// index 0 = vektor 32, index 223 = vektor 255.
extern void (*const isr_default_table[224])();

// ============================================================
// IDT Entry 64-bit (16 byte per entry — standar x86-64)
// Layout sesuai Intel Manual Vol.3 Table 6-2
// ============================================================
struct idt_entry {
    uint16_t base_lo;    // Bits 15:0 dari handler address
    uint16_t sel;        // Code segment selector
    uint8_t  ist;        // Interrupt Stack Table offset (0 = tidak pakai IST)
    uint8_t  flags;      // Type & Attributes (0x8E = interrupt gate, 0xEE = trap DPL3)
    uint16_t base_mid;   // Bits 31:16 dari handler address
    uint32_t base_hi;    // Bits 63:32 dari handler address
    uint32_t reserved;   // Harus 0
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;       // Pointer 64-bit ke tabel IDT
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr idtp;

extern void idt_flush(uint64_t ptr);

// ------------------------------------------------------------
// Flag gate.
//
//   0x8E = present, DPL=0, 64-bit interrupt gate (IF di-clear saat masuk)
//   0xEE = present, DPL=3, 64-bit interrupt gate
//   0x8F = present, DPL=0, 64-bit TRAP gate (IF dibiarkan)
//
// ARCH-3: exception yang harus bisa dipicu dari ring-3 (breakpoint, overflow,
// BOUND, invalid opcode) memakai DPL=3 supaya instruksi `int3`/`into`/
// `bound`/`ud2` di user space menghasilkan exception yang benar, bukan #GP.
// Ini juga yang membuat debugger user-space mungkin.
// ------------------------------------------------------------
#define GATE_INT_DPL0 0x8E
#define GATE_INT_DPL3 0xEE
#define GATE_TRAP_DPL0 0x8F

// ARCH-2: ist_index 0 = tanpa IST; 1..7 = KZ_IST_* (lihat include/arch.h).
void idt_set_gate_ist(uint8_t num, uint64_t base, uint16_t sel,
                      uint8_t flags, uint8_t ist_index) {
    idt[num].base_lo  = (uint16_t)(base & 0xFFFF);
    idt[num].base_mid = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].base_hi  = (uint32_t)((base >> 32) & 0xFFFFFFFF); // FIX: bukan >> 64 (UB!)
    idt[num].sel      = sel;
    idt[num].ist      = (uint8_t)(ist_index & 0x07);
    idt[num].flags    = flags;
    idt[num].reserved = 0;
}

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt_set_gate_ist(num, base, sel, flags, KZ_IST_NONE);
}

// ------------------------------------------------------------
// ARCH-3: vektor 32..255 yang tidak punya driver/handler khusus.
//
// Semuanya di-resolve ke default stub. Simbol `isrN_stub` untuk N di
// 32..127, 129..239, 241..252, 254..255 di-emit oleh blok %rep di
// exceptions.asm, jadi tabel di bawah cukup menunjuk alamatnya.
//
// Yang DIKECUALIKAN (punya file sendiri):
//   128 = syscall, 240 = LAPIC timer, 253 = IPI reschedule
// ------------------------------------------------------------
extern void isr32_stub();   extern void isr33_stub();
extern void isr44_stub();
extern void isr255_stub();

// Tabel default stub vektor 32..255. Slot yang punya handler khusus diisi
// ulang setelahnya di init_idt(), jadi tabel ini boleh berisi pointer default
// untuk semua vektor.
extern void (*const isr_default_table[224])();

void init_idt() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint64_t)&idt;

    memset(&idt, 0, sizeof(struct idt_entry) * 256);

    // --- EXCEPTION HANDLERS ---
    // ARCH-3: DPL=3 untuk exception yang bisa/ boleh dipicu user.
    idt_set_gate(KZ_VEC_DE,  (uint64_t)isr0_stub,  KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_DB,  (uint64_t)isr1_stub,  KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    // ARCH-2: NMI pakai IST — NMI bisa datang saat stack sedang korup.
    idt_set_gate_ist(KZ_VEC_NMI, (uint64_t)isr2_stub, KZ_SEL_KERNEL_CS,
                     GATE_INT_DPL0, KZ_IST_NMI);
    idt_set_gate(KZ_VEC_BP,  (uint64_t)isr3_stub,  KZ_SEL_KERNEL_CS, GATE_INT_DPL3);
    idt_set_gate(KZ_VEC_OF,  (uint64_t)isr4_stub,  KZ_SEL_KERNEL_CS, GATE_INT_DPL3);
    idt_set_gate(KZ_VEC_BR,  (uint64_t)isr5_stub,  KZ_SEL_KERNEL_CS, GATE_INT_DPL3);
    idt_set_gate(KZ_VEC_UD,  (uint64_t)isr6_stub,  KZ_SEL_KERNEL_CS, GATE_INT_DPL3);
    idt_set_gate(KZ_VEC_NM,  (uint64_t)isr7_stub,  KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    // ARCH-2: #DF WAJIB IST — ini justru fault yang terjadi saat stack rusak.
    idt_set_gate_ist(KZ_VEC_DF, (uint64_t)isr8_stub, KZ_SEL_KERNEL_CS,
                     GATE_INT_DPL0, KZ_IST_DF);
    idt_set_gate(KZ_VEC_TS,  (uint64_t)isr10_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_NP,  (uint64_t)isr11_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_SS,  (uint64_t)isr12_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_GP,  (uint64_t)isr13_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_PF,  (uint64_t)isr14_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_AC,  (uint64_t)isr17_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    // ARCH-2: #MC pakai IST — machine check bisa datang tanpa stack valid.
    idt_set_gate_ist(KZ_VEC_MC, (uint64_t)isr18_stub, KZ_SEL_KERNEL_CS,
                     GATE_INT_DPL0, KZ_IST_MC);
    idt_set_gate(KZ_VEC_XM,  (uint64_t)isr19_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_VE,  (uint64_t)isr20_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_CP,  (uint64_t)isr21_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_VC,  (uint64_t)isr29_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_SX,  (uint64_t)isr30_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL0);

    // --- ARCH-3: semua vektor 32..255 dapat stub default dulu ---
    // Ini menutup lubang "vektor tak terpasang → IDT nol → #GP → #DF".
    // Termasuk vektor 255 (spurious LAPIC), #MC-adjacent, dan IRQ PIC yang
    // tidak punya driver.
    for (uint32_t v = 32; v < 256; v++) {
        idt_set_gate((uint8_t)v, (uint64_t)isr_default_table[v - 32],
                     KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    }

    // --- HARDWARE IRQ (setelah PIC remap: Master → 0x20..0x27, Slave → 0x28..0x2F) ---
    idt_set_gate(KZ_VEC_IRQ0,   (uint64_t)timer_isr_stub,        KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_IRQ1,   (uint64_t)keyboard_isr_stub,     KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_IRQ12,  (uint64_t)mouse_isr_stub,        KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_LAPIC_TIMER,   (uint64_t)lapic_timer_isr_stub,  KZ_SEL_KERNEL_CS, GATE_INT_DPL0);
    idt_set_gate(KZ_VEC_LAPIC_RESCHED, (uint64_t)reschedule_isr_stub,   KZ_SEL_KERNEL_CS, GATE_INT_DPL0);

    // --- SYSCALL (DPL=3 agar Ring 3 bisa panggil via int 0x80) ---
    // Trap gate (0xEF) TIDAK dipakai: kita ingin IF=0 selama handler supaya
    // syscall tidak re-entrant di stack yang sama. Kode kernel yang butuh
    // interrupt menyalakannya sendiri (sti;hlt di wait loop).
    idt_set_gate(KZ_VEC_SYSCALL, (uint64_t)isr128_stub, KZ_SEL_KERNEL_CS, GATE_INT_DPL3);

    idt_flush((uint64_t)&idtp);
}

void idt_load(void) {
    idt_flush((uint64_t)&idtp);
}
