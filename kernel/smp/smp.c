#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "smp.h"
#include "lapic.h"
#include "task.h"

extern void gdt_load(void);
extern void idt_load(void);
// Detail LAPIC/SMP = diagnostik, bukan boot-status console → serial COM1 saja.
extern void serial_print(const char* s);
static void smp_serial_num(uint32_t v) {
    char b[12]; int n = 0;
    if (v == 0) { serial_print("0"); return; }
    char t[12]; int i = 0;
    while (v > 0 && i < 11) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) b[n++] = t[--i];
    b[n] = '\0';
    serial_print(b);
}

// ============================================================
// SMP BRING-UP AWAL
//
// Tahap awal: AP/core lain dibuat online, masuk idle loop scheduler, dan
// menerima LAPIC timer/IPI reschedule. Subsystem besar masih belum semua
// SMP-safe, jadi task 0 tetap ditahan di BSP.
// ============================================================

#define SMP_AP_STACK_SIZE   16384

typedef struct {
    uint64_t stack_top;      // offset 0, dibaca oleh arch/x86/smp_ap_entry.asm
    uint32_t cpu_index;
    uint32_t processor_id;
    uint32_t lapic_id;
    volatile uint32_t online;
    uint8_t reserved[4];
    __attribute__((aligned(16))) uint8_t stack[SMP_AP_STACK_SIZE];
} smp_cpu_state_t;

// Kontrak ABI dengan arch/x86/smp_ap_entry.asm: stack_top WAJIB di offset 0.
_Static_assert(offsetof(smp_cpu_state_t, stack_top) == 0,
               "smp_ap_entry.asm membaca stack_top di offset 0");

static smp_cpu_state_t smp_cpu_states[SMP_MAX_CPUS];
static percpu_t smp_percpu[SMP_MAX_CPUS];
static volatile uint32_t smp_cpu_online_count = 1; // BSP sudah online
static volatile uint32_t smp_log_lock = 0;

extern void smp_ap_entry(struct limine_mp_info *cpu);

static void smp_spin_lock(volatile uint32_t *lock) {
    for (;;) {
        uint32_t taken = 1;
        __asm__ volatile(
            "lock xchg %0, %1"
            : "+r"(taken), "+m"(*lock)
            :
            : "memory"
        );
        if (taken == 0) return;
        while (*lock) {
            __asm__ volatile("pause");
        }
    }
}

static void smp_spin_unlock(volatile uint32_t *lock) {
    __asm__ volatile("" ::: "memory");
    *lock = 0;
}

static uint32_t smp_atomic_inc(volatile uint32_t *value) {
    uint32_t old = 1;
    __asm__ volatile(
        "lock xadd %0, %1"
        : "+r"(old), "+m"(*value)
        :
        : "memory"
    );
    return old + 1;
}

void smp_register_cpu(uint32_t cpu_id, uint32_t processor_id, uint32_t lapic_id, uint32_t online) {
    if (cpu_id >= SMP_MAX_CPUS) return;

    smp_percpu[cpu_id].processor_id = processor_id;
    smp_percpu[cpu_id].lapic_id = lapic_id;
    smp_percpu[cpu_id].online = online;
    smp_percpu[cpu_id].reschedule_pending = 0;
    smp_percpu[cpu_id].idle_ticks = 0;
    smp_percpu[cpu_id].scheduler_ticks = 0;
    smp_percpu[cpu_id].current_cr3 = 0;  // Will be set when CPU comes online
}

void smp_set_cpu_online(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) return;

    if (smp_percpu[cpu_id].online == 0) {
        smp_percpu[cpu_id].online = 1;
        smp_atomic_inc(&smp_cpu_online_count);
    }
}

percpu_t* smp_get_cpu(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) return NULL;
    return &smp_percpu[cpu_id];
}

percpu_t* smp_current_cpu(void) {
    return smp_get_cpu(smp_current_cpu_index());
}

void smp_note_idle_tick(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) return;
    __asm__ volatile("lock incq %0" : "+m"(smp_percpu[cpu_id].idle_ticks) :: "memory");
}

void smp_note_scheduler_tick(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) return;
    __asm__ volatile("lock incq %0" : "+m"(smp_percpu[cpu_id].scheduler_ticks) :: "memory");
}

void smp_mark_reschedule(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) return;
    smp_percpu[cpu_id].reschedule_pending = 1;
}

void smp_clear_reschedule(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) return;
    smp_percpu[cpu_id].reschedule_pending = 0;
}

int smp_reschedule_pending(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) return 0;
    return smp_percpu[cpu_id].reschedule_pending != 0;
}

uint32_t smp_current_cpu_index(void) {
    uint32_t current_lapic = lapic_id();

    for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
        if (smp_percpu[i].online && smp_percpu[i].lapic_id == current_lapic) {
            return i;
        }
    }

    return 0;
}

uint32_t smp_online_cpu_count(void) {
    return smp_cpu_online_count;
}

void smp_ap_main(struct limine_mp_info *cpu, smp_cpu_state_t *state) {
    __asm__ volatile("cli");

    gdt_load();
    idt_load();
    lapic_init_ap();

    // FIX_005 Tahap 1: TR bersifat per-core — AP wajib load TSS miliknya
    // sendiri dan mengisi RSP0 ke syscall stack permanen CPU ini.
    extern void tss_load_cpu(uint32_t cpu);
    extern void tss_set_rsp0(uint32_t cpu, uint64_t rsp0);
    tss_set_rsp0(state->cpu_index, task_syscall_stack_top(state->cpu_index));
    tss_load_cpu(state->cpu_index);

    // FIX_005 Tahap 4: CR4.SMEP/SMAP & CR0.WP bersifat per-core — AP wajib
    // mengaktifkan miliknya sendiri (BSP sudah di kernel_main).
    {
        extern void cpu_enable_smap_smep(void);
        extern int  cpu_verify_wp(void);
        cpu_enable_smap_smep();
        cpu_verify_wp();
    }

    // Record this AP's current CR3 (inherited from BSP via Limine)
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    smp_percpu[state->cpu_index].current_cr3 = cr3 & 0xFFFFFFFFFFFFF000ULL;

    state->online = 1;
    smp_set_cpu_online(state->cpu_index);

#ifdef HEAP_WATCH_DEBUG
    // DR register bersifat per-CPU: setiap AP harus memasang watchpoint-nya
    // sendiri, kalau tidak writer dari AP ini tak pernah ketangkap.
    extern void heap_watch_set(uint64_t addr, int len_bytes);
    extern uint64_t heap_watch_target_addr;
    heap_watch_set(heap_watch_target_addr, 4);
#endif

    smp_spin_lock(&smp_log_lock);
    serial_print("[smp] CPU #");
    smp_serial_num(cpu->processor_id);
    serial_print(" online (lapic=");
    smp_serial_num(cpu->lapic_id);
    serial_print(")\n");
    smp_spin_unlock(&smp_log_lock);

    // FIX_001: tinggalkan stack awal dari Limine — idle loop berjalan di
    // idle stack permanen per-CPU (dialokasikan di tasking_init).
    task_switch_to_idle_stack(task_idle_stack_top(state->cpu_index));
}

void smp_init(struct limine_mp_response *mp) {
    smp_register_cpu(0, 0, lapic_id(), 1);

    if (mp == NULL || mp->cpu_count == 0 || mp->cpus == NULL) {
        serial_print("[smp] MP response unavailable; running single-core\n");
        return;
    }

    serial_print("[smp] BSP lapic=");
    smp_serial_num(mp->bsp_lapic_id);
    serial_print(", CPUs reported=");
    smp_serial_num(mp->cpu_count);
    serial_print("\n");

    uint32_t ap_slot = 0;
    for (uint64_t i = 0; i < mp->cpu_count && ap_slot + 1 < SMP_MAX_CPUS; i++) {
        struct limine_mp_info *cpu = mp->cpus[i];
        if (cpu == NULL) continue;

        if (cpu->lapic_id == mp->bsp_lapic_id) {
            smp_cpu_states[0].cpu_index = 0;
            smp_cpu_states[0].processor_id = cpu->processor_id;
            smp_cpu_states[0].lapic_id = cpu->lapic_id;
            smp_cpu_states[0].online = 1;
            smp_register_cpu(0, cpu->processor_id, cpu->lapic_id, 1);
            continue;
        }

        ap_slot++;
        smp_cpu_state_t *state = &smp_cpu_states[ap_slot];
        state->cpu_index = ap_slot;
        state->processor_id = cpu->processor_id;
        state->lapic_id = cpu->lapic_id;
        state->online = 0;
        state->stack_top = ((uint64_t)&state->stack[SMP_AP_STACK_SIZE]) & ~0xFULL;
        smp_register_cpu(ap_slot, cpu->processor_id, cpu->lapic_id, 0);

        cpu->extra_argument = (uint64_t)state;
        __asm__ volatile("" ::: "memory");
        cpu->goto_address = smp_ap_entry;
    }

    uint64_t wait_ticks = 0;
    while (smp_cpu_online_count < (uint32_t)(ap_slot + 1) && wait_ticks < 10000000ULL) {
        wait_ticks++;
        __asm__ volatile("pause");
    }

    serial_print("[smp] Online CPUs: ");
    smp_serial_num(smp_cpu_online_count);
    serial_print("/");
    smp_serial_num(ap_slot + 1);
    serial_print("\n");
}
