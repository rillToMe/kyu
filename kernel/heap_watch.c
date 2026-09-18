#ifdef HEAP_WATCH_DEBUG
#include "heap_watch.h"
#include "serial.h"
#include "smp.h"
#include <stdint.h>

// Alamat header remainder yang korup (dari panic "HEAP CORRUPTION" terakhir).
// Cross-check setelah rebuild: di log, request=0x2CA30 harus menunjukkan
// [BEFORE SPLIT] current + 0x20 + request == alamat ini. Jika bergeser,
// ganti konstanta ini lalu rebuild.
uint64_t heap_watch_target_addr = 0xFFFF9000002B3A50ULL;

// Batasi jumlah hit yang dilog per CPU agar writer tight-loop tidak
// membanjiri serial. Setelah tercapai, DR0 di-disarm di CPU tersebut.
#define HEAP_WATCH_MAX_HITS 64
static volatile uint64_t heap_watch_hits = 0;
static volatile uint32_t heap_watch_muted = 0;

void serial_print_hex(uint64_t v) {
    const char* d = "0123456789ABCDEF";
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) { buf[i] = d[v & 0xF]; v >>= 4; }
    serial_print(buf);
}

void heap_watch_set(uint64_t addr, int len_bytes) {
    int len_enc = (len_bytes == 8) ? 2 : (len_bytes == 4) ? 3 : (len_bytes == 2) ? 1 : 0;
    uint64_t dr7;
    __asm__ volatile("mov %%dr7, %0" : "=r"(dr7));
    dr7 &= ~(0xFULL << 16);          // bersihkan RW0 + LEN0
    dr7 |= (1ULL << 8) | (1ULL << 9); // LE0|GE0: exact address match
    dr7 |= (1ULL << 0);              // L0: enable breakpoint 0 (lokal CPU ini)
    dr7 |= (1ULL << 16);             // RW0 = 01: trigger on data write
    dr7 |= ((uint64_t)len_enc << 18);
    __asm__ volatile("mov %0, %%dr0" : : "r"(addr));
    __asm__ volatile("mov %0, %%dr7" : : "r"(dr7));
}

static void heap_watch_disarm(void) {
    uint64_t dr7;
    __asm__ volatile("mov %%dr7, %0" : "=r"(dr7));
    dr7 &= ~1ULL;
    __asm__ volatile("mov %0, %%dr7" : : "r"(dr7));
}

extern uint64_t hhdm_offset;
extern uint64_t vmm_get_kernel_pml4_phys(void);

// Walk 4-level page tables (basis cr3_phys) untuk va. Return PTE mentah
// level terdalam yang present (0 jika ada level not-present). Diagnostik.
static uint64_t heap_watch_walk_pte(uint64_t va, uint64_t cr3_phys) {
    const int shifts[4] = {39, 30, 21, 12};
    uint64_t* tbl = (uint64_t*)((cr3_phys & 0x000FFFFFFFFFF000ULL) + hhdm_offset);
    uint64_t e = 0;
    for (int lvl = 0; lvl < 4; lvl++) {
        e = tbl[(va >> shifts[lvl]) & 0x1FF];
        if (!(e & 1)) return 0;
        if (lvl < 3 && (e & (1ULL << 7))) return e;  // huge page di level ini
        tbl = (uint64_t*)((e & 0x000FFFFFFFFFF000ULL) + hhdm_offset);
    }
    return e;
}

// Dipanggil dari exception_handler untuk vector #DB, lalu RETURN (eksekusi
// dilanjutkan via iretq di isr1_stub). Data breakpoint adalah trap: RIP di
// frame menunjuk instruksi SETELAH store yang memicu — store penulis ada
// tepat sebelum RIP ini (cari via objdump -d build/myos.bin).
//
// Yang menulis nilai != HEAP_MAGIC ke field magic adalah kandidat corruptor;
// tulisan == HEAP_MAGIC adalah header split/expand yang sah dari allocator.
void heap_watch_db_handler(registers_t *r) {
    uint64_t dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));

    // Bukan watchpoint DR0 kita (mis. single-step) — abaikan.
    if ((dr6 & 0x1) == 0) return;

    // Reset sticky bits DR6 (reserved bits harus ditulis 1).
    __asm__ volatile("mov %0, %%dr6" : : "r"(0xFFFF0FF0ULL));

    uint64_t n = heap_watch_hits;
    if (n >= HEAP_WATCH_MAX_HITS) {
        if (!heap_watch_muted) {
            heap_watch_muted = 1;
            serial_print("\n[W0] max hits tercapai - DR0 disarm di CPU ini\n");
        }
        heap_watch_disarm();
        return;
    }
    heap_watch_hits = n + 1;

    uint32_t val = *(volatile uint32_t*)heap_watch_target_addr;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    serial_print("\n[W0] n=");
    serial_print_hex(n);
    serial_print(" cpu=");
    serial_print_hex(smp_current_cpu_index());
    serial_print(" task=");
    serial_print_hex((uint64_t)smp_current_task_id());
    serial_print(" rip=");
    serial_print_hex(r->rip);
    serial_print(" val=");
    serial_print_hex(val);
    serial_print(val == 0xDEADC0DE ? " (hdr)" : "  <== SUSPECT");
    serial_print(" cr3=");
    serial_print_hex(cr3);
    serial_print(" rsp=");
    serial_print_hex(r->rsp);
    // Bukti fisik: PTE + alamat fisik di balik target saat ini.
    uint64_t pte = heap_watch_walk_pte(heap_watch_target_addr, cr3);
    serial_print(" pte=");
    serial_print_hex(pte);
    if (pte & 1) {
        serial_print(" pa=");
        serial_print_hex((pte & 0x000FFFFFFFFFF000ULL) |
                         (heap_watch_target_addr & 0xFFFULL));
    }
    serial_print(" kpml4=");
    serial_print_hex(vmm_get_kernel_pml4_phys());
    serial_print("\n");
}
#endif
