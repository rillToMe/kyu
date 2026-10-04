#ifndef SMP_H
#define SMP_H

#include <stdint.h>

#define SMP_MAX_CPUS 16

typedef struct {
    // ARCH-9: WAJIB field pertama (offset 0) — smp_current_cpu_index()
    // membacanya lewat GS base sebagai word pertama blok per-CPU ini.
    uint32_t cpu_index;
    uint32_t processor_id;
    uint32_t lapic_id;
    volatile uint32_t online;
    volatile uint32_t reschedule_pending;
    volatile uint64_t idle_ticks;
    volatile uint64_t scheduler_ticks;
    volatile uint64_t current_cr3;    // Physical CR3 value loaded on this CPU
} percpu_t;

void smp_register_cpu(uint32_t cpu_id, uint32_t processor_id, uint32_t lapic_id, uint32_t online);
void smp_set_cpu_online(uint32_t cpu_id);

uint32_t smp_current_cpu_index(void);
uint32_t smp_online_cpu_count(void);

// ARCH-9: set GS base CPU ini ke blok per-CPU-nya (jalur cepat
// smp_current_cpu_index). Dipanggil BSP + setiap AP saat bring-up.
void smp_set_gs_base(uint32_t cpu_id);
int  smp_gs_base_active(void);

percpu_t* smp_get_cpu(uint32_t cpu_id);
percpu_t* smp_current_cpu(void);

void smp_note_idle_tick(uint32_t cpu_id);
void smp_note_scheduler_tick(uint32_t cpu_id);

void smp_mark_reschedule(uint32_t cpu_id);
void smp_clear_reschedule(uint32_t cpu_id);
int  smp_reschedule_pending(uint32_t cpu_id);

// Bring-up semua AP via Limine MP response (boleh NULL → single-core).
struct limine_mp_response;
void smp_init(struct limine_mp_response *mp);

#endif
