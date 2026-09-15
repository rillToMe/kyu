// ============================================================
// kernel/sched/lifecycle.c — Task Lifecycle, Kyuzen OS
//
// tasking_init, create_task(_prio), task_exit, plus stack permanen per-CPU
// (idle stack FIX_001 & syscall stack FIX_005). Slot-reaper di create_task_prio
// dan zero-ownership stack di task_exit adalah dua sisi dari satu invarian —
// sengaja dijaga dalam satu file.
// ============================================================

#include "task.h"
#include "proc.h"
#include "uheap.h"
#include "heap.h"
#include "spinlock.h"
#include "smp.h"
#include "lapic.h"
#include "paging.h"
#include "vfs.h"
#include "sched_internal.h"
#include <stddef.h>

extern void kprint(const char* str);
extern void kprint_num(uint64_t num);
extern void kernel_panic(const char* title, const char* desc, uint64_t code);
extern void tss_set_rsp0(uint32_t cpu, uint64_t rsp0);

// FIX_001: idle stack permanen per-CPU — dialokasikan sekali di tasking_init,
// tidak pernah di-free. Lihat task.h untuk kontraknya.
static uint64_t idle_stack_tops[SMP_MAX_CPUS];

uint64_t task_idle_stack_top(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) cpu_id = 0;
    return idle_stack_tops[cpu_id];
}

// FIX_005 Tahap 1: syscall stack permanen per-CPU — RSP0 milik TSS setiap
// CPU saat transisi ring-3 → ring-0 (int 0x80, IRQ, exception dari app).
// 16 KB (2× task stack) karena rantai syscall bisa dalam (exec → ELF → KFS).
#define SYSCALL_STACK_SIZE 16384
static uint64_t syscall_stack_tops[SMP_MAX_CPUS];

uint64_t task_syscall_stack_top(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) cpu_id = 0;
    return syscall_stack_tops[cpu_id];
}

// Alokasi idle + syscall stack per-CPU dan isi RSP0 milik BSP.
// AP mengisi RSP0-nya sendiri di smp_ap_main (stack sudah siap dari sini).
void sched_stacks_init(void) {
    // FIX_001: idle stack permanen per-CPU. task_exit() pindah ke stack ini
    // sebelum meng-kfree stack task DEAD — menutup race UAF antara CPU yang
    // masih idling di stack lama dan slot reaper di create_task (CPU lain).
    for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
        void* s = kmalloc(TASK_STACK_SIZE);
        if (!s) {
            kernel_panic("TASK INIT", "OOM alokasi per-CPU idle stack", i);
        }
        idle_stack_tops[i] = ((uint64_t)s + TASK_STACK_SIZE) & ~15ULL;
    }

    // FIX_005 Tahap 1: syscall stack permanen per-CPU + isi RSP0 milik BSP.
    for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
        void* s = kmalloc(SYSCALL_STACK_SIZE);
        if (!s) {
            kernel_panic("TASK INIT", "OOM alokasi per-CPU syscall stack", i);
        }
        syscall_stack_tops[i] = ((uint64_t)s + SYSCALL_STACK_SIZE) & ~15ULL;
    }
    tss_set_rsp0(0, syscall_stack_tops[0]);
}

// Dipanggil dari task_exit_via_idle (asm) SETELAH RSP pindah ke idle stack.
// Baru di sinilah stack task DEAD boleh di-free.
void task_exit_finish_on_idle(void* old_stack_base) {
    if (old_stack_base != NULL) {
        kfree(old_stack_base);
    }
    scheduler_idle_loop();
}

// String copy helper (tidak bisa include string.h di kernel)
static void task_strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// P0 Phase 1 — credential accessors. Task-local reads, no locks:
// each cred is written only at creation (under scheduler_lock, before
// READY) or by its own task via sys_set_uid (under scheduler_lock);
// 32-bit aligned reads are atomic on x86. Remote reads are advisory,
// same contract as cpu_current_task[].
uint32_t cred_task_uid(const task_t* t) { return t ? t->cred.uid : CRED_ROOT_UID; }
uint32_t cred_task_gid(const task_t* t) { return t ? t->cred.gid : CRED_ROOT_GID; }

uint32_t cred_current_uid(void) {
    int id = smp_current_task_id();
    if (id < 0 || id >= task_count) return CRED_ROOT_UID; // idle/early = kernel
    return tasks[id].cred.uid;
}

uint32_t cred_current_gid(void) {
    int id = smp_current_task_id();
    if (id < 0 || id >= task_count) return CRED_ROOT_GID; // idle/early = kernel
    return tasks[id].cred.gid;
}

int cred_current_is_root(void) {
    return cred_current_uid() == CRED_ROOT_UID;
}

// Inherit creator cred + parent for a new slot. Caller holds scheduler_lock.
// Parent is the running task on this CPU; it cannot be DEAD under us.
static void task_cred_inherit_locked(int slot) {
    int parent = smp_current_task_id();
    if (parent >= 0 && parent < task_count) {
        cred_inherit(&tasks[slot].cred, &tasks[parent].cred);
        tasks[slot].parent_id = parent;
    } else {
        // No creator (early boot / idle kthread path): kernel identity,
        // explicit — not a silent privilege grant to a user task.
        tasks[slot].cred.uid = CRED_ROOT_UID;
        tasks[slot].cred.gid = CRED_ROOT_GID;
        tasks[slot].parent_id = PROC_NO_PARENT;
    }
}

static void task_entry_trampoline(void (*func)(void)) {
    if (func != NULL) {
        func();
    }
    task_exit();
}

// ============================================================
// tasking_init — Daftarkan task 0 (kernel main) sebagai current task
// ============================================================
void tasking_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_DEAD;
        tasks[i].rsp   = 0;
        tasks[i].pml4_phys = PHYS_NULL;
        tasks[i].cookie = 0;
        tasks[i].wake_at_ms = 0;
        tasks[i].priority = PRIO_NORMAL;
        tasks[i].enqueue_ms = 0;
        tasks[i].uheap_brk = 0;
        tasks[i].uheap_regions = NULL;
        tasks[i].kind = TASK_KIND_KERNEL;
        tasks[i].parent_id = PROC_NO_PARENT;
        tasks[i].exit_code = 0;
        tasks[i].argc = 0;
        tasks[i].exit_reason = 0;
        tasks[i].kill_pending = 0;
    }

    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        cpu_current_task[i] = -1;
    }
    runq_init();

    sched_stacks_init();

    // P0 Phase 2: process wait queue (parent/child exit synchronization).
    {
        extern void proc_init(void);
        proc_init();
    }

    // Task 0 = kernel main thread yang sedang berjalan
    // RSP-nya akan diisi oleh schedule() pada preemption pertama
    tasks[0].id         = 0;
    tasks[0].state      = TASK_RUNNING;
    tasks[0].stack_base = 0;   // Kernel stack, jangan di-free
    tasks[0].pml4_phys  = PHYS_NULL;  // Uses boot PML4
    tasks[0].cookie     = 0;          // Kernel task: no cookie
    tasks[0].wake_at_ms = 0;
    tasks[0].priority   = PRIO_NORMAL;
    tasks[0].enqueue_ms = 0;
    tasks[0].kind       = TASK_KIND_KERNEL;
    tasks[0].cred.uid   = CRED_ROOT_UID;   // P0 Phase 1: kernel/root identity
    tasks[0].cred.gid   = CRED_ROOT_GID;
    tasks[0].parent_id  = PROC_NO_PARENT;  // P0 Phase 2: no parent (reaper placeholder)
    tasks[0].exit_code  = 0;
    tasks[0].argc       = 0;
    tasks[0].exit_reason  = 0;
    tasks[0].kill_pending = 0;
    task_strncpy(tasks[0].name, "kmain", 16);

    current_task = 0;
    task_count   = 1;
    cpu_current_task[0] = 0;

    // Record boot CR3 for percpu tracking
    phys_addr_t boot_cr3 = vmm_read_cr3();
    percpu_t *bsp = smp_get_cpu(0);
    if (bsp != NULL) {
        bsp->current_cr3 = (uint64_t)boot_cr3;
    }
}

// ============================================================
// create_task — Buat task baru dengan Fake ISR Frame
//
// Fake ISR Frame adalah representasi dari "seolah-olah task ini
// baru saja di-interrupt oleh IRQ0". Saat scheduler memilih task ini,
// POPA64 + IRETQ akan me-restore frame ini dan melompat ke func.
//
// Layout Fake ISR Frame (dari TINGGI ke RENDAH, dibangun dari stack_top turun):
//   stack_top → [SS][RSP][RFLAGS][CS][RIP][error_code][int_num]
//               [rax][rbx][rcx][rdx][rbp][rsi][rdi][r8..r15]
//                                                         ↑ RSP task (frame pointer)
// ============================================================
void create_task(void (*func)(void), const char* name) {
    create_task_prio(func, name, PRIO_NORMAL);
}

void create_task_prio(void (*func)(void), const char* name, uint8_t priority) {
    uint32_t kick_cpus[SMP_MAX_CPUS];
    uint32_t kick_count = 0;

    // Alokasi stack baru
    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack) return;

    uint64_t stack_top = (uint64_t)stack + TASK_STACK_SIZE;

    // Zero out stack untuk keamanan
    for (int i = 0; i < TASK_STACK_SIZE; i++) stack[i] = 0;

    // Bangun Fake ISR Frame dari atas stack ke bawah
    // Setiap `*(--p)` = PUSH (decrement pointer, lalu isi nilai)
    uint64_t* p = (uint64_t*)stack_top;

    // ── CPU auto-push (5 slot, urutan tinggi→rendah) ──
    *(--p) = 0x10ULL;            // SS  = kernel data segment
    *(--p) = stack_top;          // RSP = top of this task's stack (setelah iretq)
    *(--p) = 0x202ULL;           // RFLAGS: bit 1 (reserved=1) + IF=1 (interrupt enabled)
    *(--p) = 0x08ULL;            // CS  = kernel code segment
    *(--p) = (uint64_t)task_entry_trampoline; // RIP = wrapper aman untuk task

    // ── ISR stub push (2 slot) ──
    *(--p) = 0ULL;               // error_code (stub push 0 PERTAMA = higher addr)
    *(--p) = 0ULL;               // int_num    (stub push int KEDUA = lower addr)

    // ── PUSHA64 order (15 slot): rax PERTAMA = highest, r15 TERAKHIR = RSP ──
    // Urutan harus cocok dengan PUSHA64 macro: push rax, rbx, rcx, rdx, rbp, rsi, rdi, r8..r15
    // (rax pushed first = ends up at RSP+112 = highest, r15 pushed last = RSP+0 = lowest)
    *(--p) = 0ULL;   // rax  (akan ada di [RSP+112] — lokasi return value syscall)
    *(--p) = 0ULL;   // rbx
    *(--p) = 0ULL;   // rcx
    *(--p) = 0ULL;   // rdx
    *(--p) = 0ULL;   // rbp
    *(--p) = 0ULL;   // rsi
    *(--p) = (uint64_t)func; // rdi = argumen pertama task_entry_trampoline
    *(--p) = 0ULL;   // r8
    *(--p) = 0ULL;   // r9
    *(--p) = 0ULL;   // r10
    *(--p) = 0ULL;   // r11
    *(--p) = 0ULL;   // r12
    *(--p) = 0ULL;   // r13
    *(--p) = 0ULL;   // r14
    *(--p) = 0ULL;   // r15  ← p sekarang = RSP yang akan disimpan di TCB

    uint64_t flags = spinlock_lock_irqsave(&scheduler_lock);

    int slot = -1;
    for (int i = 1; i < task_count; i++) {
        if (tasks[i].state == TASK_DEAD) {
            slot = i;
            break;
        }
    }

    if (slot < 0 && task_count < MAX_TASKS) {
        slot = task_count++;
    }

    if (slot < 0) {
        spinlock_unlock_irqrestore(&scheduler_lock, flags);
        kfree(stack);
        return;
    }

    // Fallback saja: task_exit (FIX_001) sudah me-zero-kan stack_base sebelum
    // slot DEAD terlihat, jadi cabang ini hanya menyentuh task yang dimatikan
    // di luar jalur task_exit normal.
    if (tasks[slot].stack_base != 0) {
        kfree((void*)tasks[slot].stack_base);
    }
    // FIX_005 Tahap 3: fallback yang sama untuk metadata heap user — slot
    // yang dimatikan di luar jalur normal tidak boleh mewariskan region list.
    uheap_reset(&tasks[slot]);

    // Phase 5B: slot reuse — buang sisa event queue pemilik lama sebelum
    // slot terlihat READY (task_exit normal juga sudah flush; ini defensif).
    {
        extern void flush_event_queue(int task_id);
        flush_event_queue(slot);
    }

    // p sekarang menunjuk ke r15, yang adalah RSP "benar" dari ISR frame ini
    tasks[slot].id         = (uint32_t)slot;
    tasks[slot].rsp        = (uint64_t)p;      // RSP = pointer ke r15 di fake frame
    tasks[slot].stack_base = (uint64_t)stack;  // Untuk cleanup nanti
    tasks[slot].state      = TASK_READY;
    tasks[slot].pml4_phys  = PHYS_NULL;        // Kernel task: shared PML4
    tasks[slot].cookie     = 0;
    tasks[slot].wake_at_ms = 0;
    tasks[slot].priority   = priority;
    tasks[slot].kind       = TASK_KIND_KERNEL;
    tasks[slot].exit_code  = 0;
    tasks[slot].argc       = 0;
    tasks[slot].exit_reason  = 0;
    tasks[slot].kill_pending = 0;
    task_cred_inherit_locked(slot);            // P0 Phase 1: child = parent cred
                                               // + P0 Phase 2: parent_id
    task_strncpy(tasks[slot].name, name ? name : "task", 16);

    spinlock_unlock_irqrestore(&scheduler_lock, flags);

    // P0 Phase 2: stdio per-task (fd 0/1/2 -> TTY). Outside scheduler_lock
    // (vfs has its own lock, must not nest). Slot not yet in runq, so the
    // new task cannot run before stdio is ready.
    {
        extern void vfs_task_init(int task_id);
        vfs_task_init(slot);
    }

    // Place the new task on the least-loaded CPU and wake only that one CPU.
    // This replaces the old broadcast-to-all-idle-cores wakeup, which made
    // every idle AP spin on the scheduler lock only for one to win the task.
    uint32_t target = pick_target_cpu();
    if (runq_push(target, slot) != 0) {
        // Queue full (only possible under MAX_TASKS overflow). Reclaim the slot.
        uint64_t reclaim = spinlock_lock_irqsave(&scheduler_lock);
        tasks[slot].state = TASK_DEAD;
        tasks[slot].rsp   = 0;
        spinlock_unlock_irqrestore(&scheduler_lock, reclaim);
        kfree(stack);
        return;
    }

    // Only a remote CPU needs an IPI to preempt promptly; CPU 0 (BSP) picks the
    // task up on its own next PIT tick, and the local AP does so on its LAPIC tick.
    if (target != smp_current_cpu_index()) {
        kick_cpus[kick_count++] = target;
    }

    for (uint32_t i = 0; i < kick_count; i++) {
        smp_mark_reschedule(kick_cpus[i]);
        lapic_send_reschedule(kick_cpus[i]);
    }
}

// ============================================================
// create_user_task — Phase 5A: task ring-3 BARU untuk sys_spawn
//
// Sama dengan create_task_prio, tetapi fake ISR frame mengarah ke ring 3:
// iretq memuat CS=0x1B/SS=0x23 dengan RIP=entry ELF dan RSP=stack user,
// sehingga app mulai langsung di CPL 3 (tanpa task_entry_trampoline).
//
// pml4_phys/cookie/kind di-set DI DALAM scheduler_lock sebelum state=READY —
// scheduler membaca pml4_phys untuk CR3 switch saat task pertama dipilih;
// jika di-set setelah READY, AP lain bisa menjemput task dengan CR3 salah.
//
// P0 Phase 2: argc/argv_uaddr dimuat ke RDI/RSI (SysV main(argc,argv)).
// argv array + strings sudah di user stack AS target. Old apps
// (void main) mengabaikan RDI/RSI — backward compatible.
// ============================================================
int create_user_task(uint64_t entry_rip, uint64_t user_rsp,
                     phys_addr_t pml4_phys, uint32_t cookie, const char* name,
                     uint64_t argc, uint64_t argv_uaddr,
                     const int inherit_fds[3]) {
    uint32_t kick_cpus[SMP_MAX_CPUS];
    uint32_t kick_count = 0;

    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack) return -1;

    uint64_t stack_top = (uint64_t)stack + TASK_STACK_SIZE;
    for (int i = 0; i < TASK_STACK_SIZE; i++) stack[i] = 0;

    // Fake ISR Frame — layout identik dengan create_task_prio, bedanya:
    // SS/CS = segmen user (RPL3), RIP = entry ELF, RSP = stack user app.
    uint64_t* p = (uint64_t*)stack_top;

    // ── CPU auto-push (5 slot, urutan tinggi→rendah) ──
    *(--p) = 0x23ULL;            // SS  = user data (GDT[4] | RPL3)
    *(--p) = user_rsp;           // RSP = stack user app (dari elf_load_file)
    *(--p) = 0x202ULL;           // RFLAGS: reserved bit + IF=1
    *(--p) = 0x1BULL;            // CS  = user code (GDT[3] | RPL3)
    *(--p) = entry_rip;          // RIP = entry point ELF (main)

    // ── ISR stub push (2 slot) ──
    *(--p) = 0ULL;               // error_code
    *(--p) = 0ULL;               // int_num

    // ── PUSHA64 order (15 slot): rax pertama = highest, r15 terakhir = RSP ──
    *(--p) = 0ULL;   // rax
    *(--p) = 0ULL;   // rbx
    *(--p) = 0ULL;   // rcx
    *(--p) = 0ULL;   // rdx
    *(--p) = 0ULL;   // rbp
    *(--p) = argv_uaddr; // rsi = argv (P0 Phase 2 ABI)
    *(--p) = argc;       // rdi = argc (old void-main apps ignore both)
    *(--p) = 0ULL;   // r8
    *(--p) = 0ULL;   // r9
    *(--p) = 0ULL;   // r10
    *(--p) = 0ULL;   // r11
    *(--p) = 0ULL;   // r12
    *(--p) = 0ULL;   // r13
    *(--p) = 0ULL;   // r14
    *(--p) = 0ULL;   // r15  ← p = RSP yang disimpan di TCB

    uint64_t flags = spinlock_lock_irqsave(&scheduler_lock);

    int slot = -1;
    for (int i = 1; i < task_count; i++) {
        if (tasks[i].state == TASK_DEAD) {
            slot = i;
            break;
        }
    }

    if (slot < 0 && task_count < MAX_TASKS) {
        slot = task_count++;
    }

    if (slot < 0) {
        spinlock_unlock_irqrestore(&scheduler_lock, flags);
        kfree(stack);
        return -1;
    }

    // Fallback reaper (pola create_task_prio): slot yang dimatikan di luar
    // jalur task_exit normal tidak boleh mewariskan stack/heap metadata.
    if (tasks[slot].stack_base != 0) {
        kfree((void*)tasks[slot].stack_base);
    }
    uheap_reset(&tasks[slot]);

    // Phase 5B: slot reuse — buang sisa event queue pemilik lama sebelum
    // slot terlihat READY.
    {
        extern void flush_event_queue(int task_id);
        flush_event_queue(slot);
    }

    // Semua field terisi SEBELUM state=READY — lihat komentar di atas.
    // P0 Phase 1: child inherits caller cred. No creator (idle/early) =
    // fail safely — never silently spawn a root user task.
    int creator = smp_current_task_id();
    if (creator < 0 || creator >= task_count) {
        spinlock_unlock_irqrestore(&scheduler_lock, flags);
        kfree(stack);
        return -1;
    }
    tasks[slot].id         = (uint32_t)slot;
    tasks[slot].rsp        = (uint64_t)p;
    tasks[slot].stack_base = (uint64_t)stack;
    tasks[slot].state      = TASK_READY;
    tasks[slot].pml4_phys  = pml4_phys;
    tasks[slot].cookie     = cookie;
    tasks[slot].wake_at_ms = 0;
    tasks[slot].priority   = PRIO_NORMAL;
    tasks[slot].kind       = TASK_KIND_SPAWNED;
    tasks[slot].exit_code  = 0;
    tasks[slot].argc       = (int32_t)argc;
    tasks[slot].exit_reason  = 0;
    tasks[slot].kill_pending = 0;
    cred_inherit(&tasks[slot].cred, &tasks[creator].cred);
    tasks[slot].parent_id  = creator;   // P0 Phase 2: explicit parent
    task_strncpy(tasks[slot].name, name ? name : "app", 16);

    spinlock_unlock_irqrestore(&scheduler_lock, flags);

    // P0 Phase 2: stdio per-task. Outside scheduler_lock (see create_task_prio).
    {
        extern void vfs_task_init(int task_id);
        vfs_task_init(slot);
    }

    // P0 Phase 5: explicit stdio inheritance (sys_spawn_redir, shell
    // redirection/pipelines). Still outside scheduler_lock (vfs has its
    // own lock, must not nest); the slot is not on any run queue yet, so
    // no CPU can observe the half-inherited table. Bad fd -> release the
    // half-built child (fds + stack) and fail, never publish it.
    if (inherit_fds) {
        extern int vfs_inherit_stdio(int child, int parent, const int spec[3]);
        extern void vfs_close_all(int task_id);
        if (vfs_inherit_stdio(slot, creator, inherit_fds) != 0) {
            vfs_close_all(slot);
            uint64_t fail = spinlock_lock_irqsave(&scheduler_lock);
            tasks[slot].state = TASK_DEAD;
            tasks[slot].rsp   = 0;
            tasks[slot].stack_base = 0;
            spinlock_unlock_irqrestore(&scheduler_lock, fail);
            kfree(stack);
            return -1;
        }
    }

    uint32_t target = pick_target_cpu();
    if (runq_push(target, slot) != 0) {
        uint64_t reclaim = spinlock_lock_irqsave(&scheduler_lock);
        tasks[slot].state = TASK_DEAD;
        tasks[slot].rsp   = 0;
        tasks[slot].stack_base = 0;   // stack di-free di bawah — jangan wariskan pointer
        spinlock_unlock_irqrestore(&scheduler_lock, reclaim);
        kfree(stack);
        return -1;
    }

    if (target != smp_current_cpu_index()) {
        kick_cpus[kick_count++] = target;
    }

    for (uint32_t i = 0; i < kick_count; i++) {
        smp_mark_reschedule(kick_cpus[i]);
        lapic_send_reschedule(kick_cpus[i]);
    }

    return slot;
}

// ============================================================
// task_fork — P0 Phase 6B: duplicate a RUNNING user task.
//
// Unlike create_user_task (fresh ELF image), fork clones live state:
// full-copy address space + forged syscall-return frame (child resumes
// after the fork trap with rax=0) + shared fd table + heap metadata.
//
// All-or-nothing: any failure destroys what was built (child AS via
// vmm_destroy_address_space, kernel stack, uheap nodes, fd refs never
// taken before the fail point) and returns -1 with the parent and its
// slot state untouched. The pid is unknown to anyone until return, so
// no CPU can observe the half-built child before runq_push — same
// argument as create_user_task.
//
// parent_id: cloning task (must own a user AS). parent_rf: its live
// syscall trap frame (user registers are copied verbatim).
// Returns: child slot id (>= 0), or -1.
// ============================================================
int task_fork(int parent_id, registers_t* parent_rf) {
    if (!parent_rf) return -1;
    if (parent_id < 0 || parent_id >= MAX_TASKS) return -1;

    uint32_t kick_cpus[SMP_MAX_CPUS];
    uint32_t kick_count = 0;

    phys_addr_t parent_as = tasks[parent_id].pml4_phys;
    if (parent_as == PHYS_NULL) return -1;   // kernel task: no user AS to clone
    if (tasks[parent_id].state != TASK_RUNNING) return -1;

    // 1. Clone the address space first (heaviest step; parent untouched
    // on failure — vmm_clone_user_as only reads parent tables).
    phys_addr_t child_as = vmm_clone_user_as(parent_as);
    if (child_as == PHYS_NULL) return -1;

    // 2. Child kernel stack (never shared with the parent).
    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        vmm_destroy_address_space(child_as, 1);
        return -1;
    }
    for (int i = 0; i < TASK_STACK_SIZE; i++) stack[i] = 0;

    uint64_t flags = spinlock_lock_irqsave(&scheduler_lock);

    int slot = -1;
    for (int i = 1; i < task_count; i++) {
        if (tasks[i].state == TASK_DEAD) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && task_count < MAX_TASKS) {
        slot = task_count++;
    }
    if (slot < 0) {
        spinlock_unlock_irqrestore(&scheduler_lock, flags);
        kfree(stack);
        vmm_destroy_address_space(child_as, 1);
        return -1;
    }

    // Same slot-reuse hygiene as create_user_task.
    if (tasks[slot].stack_base != 0) {
        kfree((void*)tasks[slot].stack_base);
    }
    uheap_reset(&tasks[slot]);
    {
        extern void flush_event_queue(int task_id);
        flush_event_queue(slot);
    }

    tasks[slot].id         = (uint32_t)slot;
    tasks[slot].rsp        = 0;   // set after forging the frame below
    tasks[slot].stack_base = (uint64_t)stack;
    tasks[slot].state      = TASK_READY;
    tasks[slot].pml4_phys  = child_as;
    tasks[slot].cookie     = as_cookie_next();
    tasks[slot].wake_at_ms = 0;
    tasks[slot].priority   = PRIO_NORMAL;
    tasks[slot].kind       = TASK_KIND_SPAWNED;
    tasks[slot].exit_code  = 0;
    tasks[slot].argc       = tasks[parent_id].argc;
    tasks[slot].exit_reason  = 0;
    tasks[slot].kill_pending = 0;
    cred_inherit(&tasks[slot].cred, &tasks[parent_id].cred);
    tasks[slot].parent_id  = parent_id;
    task_strncpy(tasks[slot].name, tasks[parent_id].name, 16);

    // Heap metadata follows the cloned pages (same user vaddrs).
    if (uheap_clone(&tasks[slot], &tasks[parent_id]) != 0) {
        tasks[slot].state = TASK_DEAD;
        tasks[slot].rsp   = 0;
        tasks[slot].stack_base = 0;
        spinlock_unlock_irqrestore(&scheduler_lock, flags);
        kfree(stack);
        vmm_destroy_address_space(child_as, 1);
        return -1;
    }

    spinlock_unlock_irqrestore(&scheduler_lock, flags);

    // 3. Forge the child's trap frame: parent's user registers verbatim,
    // rax forced to 0 (the child's fork() return). Layout is registers_t
    // by construction (see isr_macro.inc) — the child returns through the
    // normal POPA64+iretq path like any syscall exit.
    {
        registers_t* cp = (registers_t*)((uint64_t)stack + TASK_STACK_SIZE -
                                          sizeof(registers_t));
        *cp = *parent_rf;
        cp->rax = 0;
        cp->int_num = 128;
        cp->error_code = 0;
        tasks[slot].rsp = (uint64_t)cp;
    }

    // 4. Whole fd table, same numbers, shared descriptions. Child table
    // must be empty (stale occupant -> fail, never clobber). No KWM
    // windows: ownership is per-task and the fresh slot has none; the
    // parent keeps its own (exit paths tear down per owner).
    {
        extern int vfs_fork_inherit(int child, int parent);
        if (vfs_fork_inherit(slot, parent_id) != 0) {
            uint64_t fail = spinlock_lock_irqsave(&scheduler_lock);
            tasks[slot].state = TASK_DEAD;
            tasks[slot].rsp   = 0;
            tasks[slot].stack_base = 0;
            spinlock_unlock_irqrestore(&scheduler_lock, fail);
            uheap_reset(&tasks[slot]);
            kfree(stack);
            vmm_destroy_address_space(child_as, 1);
            return -1;
        }
    }

    uint32_t target = pick_target_cpu();
    if (runq_push(target, slot) != 0) {
        uint64_t reclaim = spinlock_lock_irqsave(&scheduler_lock);
        tasks[slot].state = TASK_DEAD;
        tasks[slot].rsp   = 0;
        tasks[slot].stack_base = 0;
        spinlock_unlock_irqrestore(&scheduler_lock, reclaim);
        {
            extern void vfs_close_all(int task_id);
            vfs_close_all(slot);
        }
        uheap_reset(&tasks[slot]);
        kfree(stack);
        vmm_destroy_address_space(child_as, 1);
        return -1;
    }

    if (target != smp_current_cpu_index()) {
        kick_cpus[kick_count++] = target;
    }

    for (uint32_t i = 0; i < kick_count; i++) {
        smp_mark_reschedule(kick_cpus[i]);
        lapic_send_reschedule(kick_cpus[i]);
    }

    return slot;
}

void task_exit(void) {
    extern void proc_exit(int code) __attribute__((noreturn));
    proc_exit(0);
}
