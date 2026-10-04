#include <stdint.h>
#include "string.h"
#include "arch.h"
#include "smp.h"   // SMP_MAX_CPUS

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

// Di 64-bit, TSS memakan 2 blok (16 byte), jadi strukturnya lebih panjang
struct tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base; // <-- Pointer sekarang 64-bit (8 byte)
} __attribute__((packed));

// Struktur TSS 64-bit
struct tss_entry_struct {
    uint32_t reserved0;
    uint64_t rsp0;       // Stack Kernel untuk interupsi
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

// FIX_005 Tahap 1: TSS per-CPU — satu deskriptor per CPU di GDT index
// 5 + 2*cpu (tiap deskriptor 16 byte = 2 slot). Tanpa ini, dua CPU yang
// transisi ring-3→ring-0 bersamaan berbagi satu RSP0 → stack saling timpa.
#define GDT_TSS_FIRST 5
#define GDT_ENTRIES   (GDT_TSS_FIRST + 2 * SMP_MAX_CPUS)
#define TSS_SELECTOR(cpu) ((uint16_t)((GDT_TSS_FIRST + 2 * (cpu)) << 3))

volatile struct gdt_entry gdt[GDT_ENTRIES];
volatile struct gdt_ptr gp;
struct tss_entry_struct tss_entries[SMP_MAX_CPUS];

// ------------------------------------------------------------
// ARCH-2: IST stacks per-CPU.
//
// Statis (BSS) dan 16-byte aligned — bukan kmalloc — karena init_gdt()
// dipanggil di boot_phase_cpu_tables() SEBELUM heap ada. 3 × 8 KB × 16 CPU
// = 384 KB BSS; kernel image ~424 KB, jadi biaya ruangnya wajar untuk
// menghilangkan kelas bug "triple fault tanpa BSOD" sepenuhnya.
//
// Urutan ist[] di TSS: ist[0]=ist1 → KZ_IST_DF, ist[1]=ist2 → KZ_IST_NMI,
// ist[2]=ist3 → KZ_IST_MC. Lihat KZ_IST_* di include/arch.h.
// ------------------------------------------------------------
static uint8_t ist_stacks[SMP_MAX_CPUS][KZ_IST_COUNT][KZ_IST_STACK_SIZE]
    __attribute__((aligned(16)));

extern void gdt_flush(uint64_t ptr);
extern void tss_flush();
extern void tss_flush_sel(uint16_t selector);

void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access = access;
}

static void write_tss_cpu(uint32_t cpu) {
    uint64_t base = (uint64_t)&tss_entries[cpu];
    uint32_t limit = sizeof(tss_entries[cpu]);

    struct tss_descriptor* tss_desc =
        (struct tss_descriptor*)&gdt[GDT_TSS_FIRST + 2 * cpu];
    tss_desc->base_low = (base & 0xFFFF);
    tss_desc->base_middle = (base >> 16) & 0xFF;
    tss_desc->base_high = (base >> 24) & 0xFF;
    tss_desc->base_upper = (base >> 32) & 0xFFFFFFFF;
    tss_desc->limit_low = (limit & 0xFFFF);
    tss_desc->granularity = 0x00;
    tss_desc->access = 0x89; // TSS Present & Executable
    tss_desc->reserved = 0;

    memset((void*)&tss_entries[cpu], 0, sizeof(tss_entries[cpu]));
    tss_entries[cpu].rsp0 = 0;  // diisi tss_set_rsp0() setelah heap siap
    tss_entries[cpu].iomap_base = sizeof(tss_entries[cpu]);
}

// ------------------------------------------------------------
// ARCH-2: Isi slot IST TSS untuk satu CPU.
//
// Setiap stack tumbuh ke BAWAH dari ujungnya, jadi yang ditulis ke TSS
// adalah alamat ujung atas (base + size), bukan base.
// ------------------------------------------------------------
void tss_set_ist(uint32_t cpu, uint32_t ist_index, uint64_t stack_top) {
    if (cpu >= SMP_MAX_CPUS) return;
    if (ist_index < 1 || ist_index > KZ_IST_COUNT) return;

    switch (ist_index) {
        case KZ_IST_DF:  tss_entries[cpu].ist1 = stack_top; break;
        case KZ_IST_NMI: tss_entries[cpu].ist2 = stack_top; break;
        case KZ_IST_MC:  tss_entries[cpu].ist3 = stack_top; break;
        default: break;
    }
}

// Alamat ujung atas stack IST milik satu CPU (dipakai init + AP bring-up).
uint64_t tss_ist_stack_top(uint32_t cpu, uint32_t ist_index) {
    if (cpu >= SMP_MAX_CPUS) return 0;
    if (ist_index < 1 || ist_index > KZ_IST_COUNT) return 0;
    uint8_t* base = &ist_stacks[cpu][ist_index - 1][0];
    return ((uint64_t)base + KZ_IST_STACK_SIZE) & ~15ULL;
}

// Isi ketiga slot IST untuk satu CPU. Dipanggil BSP (init_gdt) dan setiap AP
// (smp_ap_main) — TSS bersifat per-core.
void tss_init_ist_cpu(uint32_t cpu) {
    if (cpu >= SMP_MAX_CPUS) return;
    for (uint32_t i = 1; i <= KZ_IST_COUNT; i++) {
        tss_set_ist(cpu, i, tss_ist_stack_top(cpu, i));
    }
}

// Isi RSP0 milik satu CPU (dipanggil setelah syscall stack-nya dialokasikan).
void tss_set_rsp0(uint32_t cpu, uint64_t rsp0) {
    if (cpu >= SMP_MAX_CPUS) return;
    tss_entries[cpu].rsp0 = rsp0;
}

// Load TR ke TSS milik CPU ini (wajib per-CPU — TR bersifat per-core).
void tss_load_cpu(uint32_t cpu) {
    if (cpu >= SMP_MAX_CPUS) return;
    tss_flush_sel(TSS_SELECTOR(cpu));
}

void init_gdt() {
    gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gp.base = (uint64_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);                // Null segment
    // GRANULARITY 0xAF: Bendera 'Long Mode' dinyalakan! (L=1, D=0)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF); // Kernel Code  (0x08)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Kernel Data  (0x10)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xAF); // User Code    (0x18, DPL=3)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User Data    (0x20, DPL=3)

    // Deskriptor TSS untuk SEMUA CPU ditulis statis di sini (alamat
    // tss_entries sudah pasti sejak link); rsp0 diisi belakangan.
    // ARCH-2: slot IST ikut diisi sekarang — tidak butuh heap.
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        write_tss_cpu(cpu);
        tss_init_ist_cpu(cpu);
    }

    gdt_flush((uint64_t)&gp);
    tss_flush(); // BSP: selector 0x28 = TSS CPU 0
}

void gdt_load(void) {
    gdt_flush((uint64_t)&gp);
}
