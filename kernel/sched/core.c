

// kernel/sched/core.c — Preemptive Scheduler Core, Kyuzen OS
//
// ARSITEKTUR: Semua context switch via Full ISR Frame.
//   - Context switch dipicu oleh PIT IRQ0 di BSP dan LAPIC timer di AP.
//   - Setiap task punya stack dengan "Fake ISR Frame" sehingga
//     POPA64 + IRETQ dari timer_isr bisa melanjutkan task tersebut.
//   - switch_task() (cooperative) DIHAPUS.
//
// FLOW:
//   Timer ISR → timer_handler/lapic_timer_handler(rsp) → schedule*(r)
//        → mov rsp, rax → POPA64 → IRETQ → task baru berjalan


#include "task.h"
#include "spinlock.h"
#include "smp.h"
#include "lapic.h"
#include "paging.h"
#include "sched_internal.h"
#include <stddef.h>


// GLOBAL STATE

task_t tasks[MAX_TASKS];
int    current_task = 0;
int    task_count   = 0;

// Guards TCB slot allocation, task_count, and per-task metadata teardown.
// NOT taken on the scheduling hot path — see cpu_current_task note below.
spinlock_t scheduler_lock = SPINLOCK_INIT;

// Per-CPU "currently running task" id (-1 = idle). Each entry is written only
// by its own CPU (schedule_on_cpu / task_exit / idle loop) plus one-time init,
// so it needs no cross-CPU lock. Remote reads (dump, panic) are advisory.
int cpu_current_task[SMP_MAX_CPUS];

const char* task_state_name(uint8_t state) {
    switch (state) {
        case TASK_READY: return "READY";
        case TASK_RUNNING: return "RUNNING";
        case TASK_SLEEPING: return "SLEEPING";
        case TASK_DEAD: return "DEAD";
        case TASK_BLOCKED: return "BLOCKED";
        case TASK_ZOMBIE: return "ZOMBIE";
        default: return "UNKNOWN";
    }
}

void scheduler_idle_loop(void) {
    for (;;) {
        uint32_t cpu_id = smp_current_cpu_index();

        // Bug 4.5: cpu_current_task[] ditulis HANYA oleh CPU-nya sendiri
        // (lihat catatan di deklarasi: "each entry is written only by its own
        // CPU") — tidak butuh scheduler_lock. Menghapus lock menghindari
        // kontensi lock pada jalur idle.
        if (cpu_id < SMP_MAX_CPUS) {
            cpu_current_task[cpu_id] = -1;
        }

        smp_note_idle_tick(cpu_id);
        __asm__ volatile("sti; hlt");
    }
}


// schedule_on_cpu — Per-CPU Run Queue Scheduler with Work-Stealing
//
// Dipanggil dari PIT (BSP) atau LAPIC timer (AP) setiap ~20ms, dan dari
// reschedule IPI saat CPU lain menaruh pekerjaan baru untuk CPU ini.
//
// Pemilihan task berikutnya (urutan prioritas):
//   1. Run queue lokal CPU ini (FIFO round-robin di dalam satu core).
//   2. Work-stealing: ambil satu task dari run queue remote yang paling sibuk.
//
// Urutan operasi menjaga invarian "satu task hanya di satu queue" dan mencegah
// sebuah task berjalan di dua CPU sekaligus:
//   - `next` di-POP lebih dulu (dihapus dari queue → kepemilikan eksklusif).
//   - Baru setelah `next` aman, task keluar (`cur`) disimpan konteksnya dan
//     di-PUSH kembali ke queue lokal supaya CPU lain boleh mencurinya.
//
// Input : registers_t* = RSP task yang sedang di-interrupt (full ISR frame)
// Output: registers_t* = RSP task berikutnya (akan di-load ke RSP di ASM)

registers_t* schedule_on_cpu(uint32_t cpu_id, registers_t* current_regs) {
    if (cpu_id >= SMP_MAX_CPUS || current_regs == NULL) return current_regs;

    // P0-FINAL: CPU-bound kill observation (noreturn when it fires).
    // Must precede ALL locking — proc_exit_kill takes the exit locks itself.
    proc_observe_kill_sched(cpu_id, current_regs);

    smp_note_scheduler_tick(cpu_id);
    smp_clear_reschedule(cpu_id);

    spinlock_lock(&scheduler_lock);

    if (task_count <= 0) {
        spinlock_unlock(&scheduler_lock);
        return current_regs;
    }

    int cur = cpu_current_task[cpu_id];
    int cur_live    = (cur >= 0 && cur < task_count);
    uint8_t cur_st  = cur_live ? tasks[cur].state : (uint8_t)TASK_DEAD;
    int cur_running = cur_live && (cur_st == TASK_RUNNING);
    // Voluntarily blocked (task_sleep_ms / block_current_task): its frame must be
    // preserved so unblock can resume it, but it must NOT go back on a run queue.
    int cur_blocked = cur_live && (cur_st == TASK_SLEEPING || cur_st == TASK_BLOCKED);
    if (!cur_running && !cur_blocked) {
        // Dead or invalid: relinquish the slot entirely.
        cur = -1;
        cpu_current_task[cpu_id] = -1;
    }

    // Secure the next task BEFORE releasing the current one. Popping removes the
    // task from its queue, giving this CPU exclusive ownership — no other CPU
    // can select it, so it can never run on two cores at once.
    int next = runq_pop(cpu_id);
    if (next < 0) {
        next = steal_task(cpu_id);
    }

    if (next < 0) {
        // Nothing else is runnable anywhere.
        //  - If current is a blocked task, save its resume frame and keep it as
        //    this CPU's current: it will hlt-loop in block_current_task until
        //    unblock flips it back to RUNNING (no double-run — it's in no queue).
        //  - If current is still RUNNING, leave it on-CPU untouched.
        if (cur_blocked) {
            tasks[cur].rsp = (uint64_t)current_regs;
        }
        spinlock_unlock(&scheduler_lock);
        return current_regs;
    }

    if (next >= task_count || tasks[next].state != TASK_READY || tasks[next].rsp == 0) {
        // Stale queue entry. The current task still owns this CPU, so leave its
        // state and queue membership unchanged.
        spinlock_unlock(&scheduler_lock);
        return current_regs;
    }

    // A valid replacement is secured. Save the outgoing task's context. A
    // RUNNING task goes back on a run queue (stealable); a blocked task keeps
    // its frame but is left off every queue.
    if (cur_blocked) {
        tasks[cur].rsp = (uint64_t)current_regs;
    }
    if (cur_running) {
        tasks[cur].rsp   = (uint64_t)current_regs;
        tasks[cur].state = TASK_READY;
        if (runq_push(cpu_id, cur) != 0) {
            tasks[cur].state = TASK_RUNNING;
            (void)runq_push(cpu_id, next);
            spinlock_unlock(&scheduler_lock);
            return current_regs;
        }
    }

    tasks[next].state = TASK_RUNNING;
    cpu_current_task[cpu_id] = next;
    if (cpu_id == 0) {
        current_task = next;
    }

    // ── RSP0 MENGIKUTI TASK (wajib sejak app ring-3 konkuren, Phase 5A) ──
    // Frame preemption task ring-3 ditulis CPU di RSP0 = TOP stack yang
    // ditunjuk TSS. Jika RSP0 global (per-CPU syscall stack bersama), dua
    // task ring-3 yang berbagi satu CPU saling MENIMPA frame satu sama lain
    // (alamat [TOP-F, TOP] identik) → resume dengan register sampah → BOSD.
    // Fix: tiap context switch, arahkan RSP0 ke stack kernel MILIK task
    // berikutnya. Task kernel lama (stack_base==0, mis. kmain) tetap memakai
    // per-CPU syscall stack seperti semula.
    {
        extern void tss_set_rsp0(uint32_t cpu, uint64_t rsp0);
        uint64_t rsp0_top = (tasks[next].stack_base != 0)
            ? tasks[next].stack_base + TASK_STACK_SIZE
            : task_syscall_stack_top(cpu_id);
        tss_set_rsp0(cpu_id, rsp0_top);
    }

    // ── CR3 SWITCH: Load next task's address space ──
    // Only CR3 changes. current_pml4 ALWAYS stays as kernel PML4.
    // User-range isolation is handled by the per-process PML4 loaded into CR3.
    {
        percpu_t *cpu = smp_get_cpu(cpu_id);

        if (tasks[next].pml4_phys != PHYS_NULL) {
            // User task: load its private PML4 into CR3
            uint64_t new_cr3 = (uint64_t)tasks[next].pml4_phys;
            if (cpu == NULL || cpu->current_cr3 != new_cr3) {
                __asm__ volatile("mov %0, %%cr3" :: "r"(new_cr3) : "memory");
                if (cpu) cpu->current_cr3 = new_cr3;
            }
        } else {
            // Kernel task: ensure kernel PML4 is in CR3
            phys_addr_t kern_phys = vmm_get_kernel_pml4_phys();
            if (kern_phys != PHYS_NULL && (cpu == NULL || cpu->current_cr3 != (uint64_t)kern_phys)) {
                __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)kern_phys) : "memory");
                if (cpu) cpu->current_cr3 = (uint64_t)kern_phys;
            }
        }
    }

    registers_t *next_regs = (registers_t*)tasks[next].rsp;
    spinlock_unlock(&scheduler_lock);
    return next_regs;
}

registers_t* schedule(registers_t* current_regs) {
    return schedule_on_cpu(0, current_regs);
}

// scheduler_remove_task — P0 Phase 3: purge a task id from every run queue.
//
// READY-only: a READY task lives in exactly one queue and on no CPU, so
// removing it guarantees it can never be scheduled again. RUNNING tasks
// (tracked by cpu_current_task, in no queue) are deliberately untouched —
// a remote CPU may be executing that stack right now; the runner observes
// kill_pending at a safe boundary instead. BLOCKED/SLEEPING tasks are in
// no queue either (unblock_task requeues them on wake, where the kill
// observation fires first). Caller must hold scheduler_lock.
void scheduler_remove_task(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;
    for (uint32_t c = 0; c < SMP_MAX_CPUS; c++) {
        runq_remove(c, task_id);
    }
}


// smp_current_task_id — Per-CPU current task ID
//
// Returns the task ID running on the CURRENT CPU.
// Returns -1 if the CPU is idle (no task scheduled).
// SMP-safe: reads per-CPU state, not the global current_task.

int smp_current_task_id(void) {
    uint32_t cpu_id = smp_current_cpu_index();
    if (cpu_id >= SMP_MAX_CPUS) return -1;
    return cpu_current_task[cpu_id];
}


// yield — Hint bahwa task sedang idle
//
// Sejak beralih ke Preemptive, yield() tidak lagi melakukan switch
// secara langsung. Context switch terjadi via timer interrupt.
// Fungsi ini sekarang hanya mem-block CPU sampai interrupt berikutnya,
// membantu cpu_idle_tracker mendeteksi bahwa task sedang menunggu.

void yield(void) {
    // hlt: tidurkan CPU sampai interrupt berikutnya (timer/keyboard/mouse)
    // sti: pastikan interrupt enabled dulu sebelum hlt
    __asm__ volatile("sti; hlt");
}
