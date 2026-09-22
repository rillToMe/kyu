#include "syscall.h"
#include <stdint.h>
#include "task.h"
#include "proc.h"
#include "paging.h"
#include "elf.h"
#include "usercopy.h"
#include "uheap.h"
#include "kwm.h"
#include "heap.h"
#include "shell.h"     // user_shell, g_shell_return_rsp (legacy exec-chain)
#include "serial.h"    // serial_print/hex (blok debug HEAP_WATCH_DEBUG)

// Tetap lokal (tanpa owner header, dipakai >1 TU — bukan header misc baru):
// - proc_copy_in_argv (butuh ucopy_ctx_t; satu-satunya pemakai di sini)
// - flush_event_queue (kernel/sync/event.c; sync.h hanya mutex/sem/condvar)
// - flush_kbd_buffer (drivers/keyboard.c; tidak ada keyboard.h)
extern int proc_copy_in_argv(const ucopy_ctx_t* uc, uint64_t u_argv, int argc,
                             char kargv[][PROC_MAX_ARG_LEN], uint32_t* total_out);
extern void flush_event_queue(int task_id);
extern void flush_kbd_buffer(void);

// Per-address-space cookie generator: increments for each new AS.
// FIX_003: increment atomik (SMP) — dua exec bersamaan tidak boleh
// mendapat nomor cookie yang sama. Identitas cookie itu sendiri disimpan
// PER-TASK (task_t.cookie), bukan di global.
static uint32_t as_cookie_counter = 0;

// Non-static definition for task_fork (declared in proc.h): every new
// address space — spawn or fork — takes the next cookie.
uint32_t as_cookie_next(void) {
    return __sync_fetch_and_add(&as_cookie_counter, 1) + 1;
}

// P0 Phase 6C — shared ELF image loader for spawn AND execve.
//
// Loads kfname's PT_LOAD segments + fresh user stack + argv into a
// target AS that is not running anywhere. Both ELF bytes and argv are
// written through user vaddrs, so the target AS is temporarily adopted
// as the CALLER's AS (CR3 + self->pml4_phys) and restored on ALL paths.
// Returns the ELF entry point, or 0 on any failure (caller destroys the
// target AS; a partially-loaded image never becomes visible).
// self may be NULL (no TCB bookkeeping then, CR3 still switched).
static uint64_t exec_load_image(task_t* self, char* kfname, int argc,
                                char kargv[][PROC_MAX_ARG_LEN],
                                phys_addr_t target_as,
                                uint64_t* stack_top_out, uint64_t* argv_out) {
    if (stack_top_out) *stack_top_out = 0;
    if (argv_out) *argv_out = 0;
    if (target_as == PHYS_NULL) return 0;

    phys_addr_t saved_as  = self ? self->pml4_phys : PHYS_NULL;
    phys_addr_t saved_cr3 = vmm_read_cr3();
    if (self) self->pml4_phys = target_as;
    vmm_switch_pml4(target_as);

    uint64_t child_stack_top = 0;
    uint64_t entry = elf_load_file(kfname, &child_stack_top, target_as);

    uint64_t argv_uaddr = 0;
    if (entry == 0 || child_stack_top == 0 ||
        proc_build_argv(&child_stack_top, argc, kargv, &argv_uaddr) != 0) {
        entry = 0;
    }

    vmm_switch_pml4(saved_cr3);
    if (self) self->pml4_phys = saved_as;

    if (entry != 0) {
        if (stack_top_out) *stack_top_out = child_stack_top;
        if (argv_out) *argv_out = argv_uaddr;
    }
    return entry;
}

// P0 Phase 2 — shared spawn core for sys_spawn (57) and sys_spawn_argv (68).
// kfname: kernel bounce (NUL-terminated). kargv: kernel bounce strings
// [argc][PROC_MAX_ARG_LEN], already validated. Returns child pid or -1.
// Leaves caller AS/CR3 restored on all paths; child AS destroyed on failure.
// P0 Phase 5: inherit_fds names the CALLER's fds to install as the child's
// 0/1/2 (NULL = fresh TTY everywhere). Installed after slot assignment but
// before the child is visible to the scheduler — never racy.
static int64_t spawn_common(char* kfname, int argc, char kargv[][PROC_MAX_ARG_LEN],
                            const int inherit_fds[3]) {
    if (!proc_argc_valid(argc)) return -1;

    phys_addr_t child_pml4 = vmm_create_address_space();
    if (child_pml4 == PHYS_NULL) return -1;

    task_t* self = syscall_current_task();
    uint64_t child_stack_top = 0, argv_uaddr = 0;
    uint64_t entry = exec_load_image(self, kfname, argc, kargv, child_pml4,
                                     &child_stack_top, &argv_uaddr);

    int64_t ret = -1;
    if (entry != 0) {
        int tid = create_user_task(entry, child_stack_top, child_pml4,
                                   as_cookie_next(), kfname,
                                   (uint64_t)argc, argv_uaddr, inherit_fds);
        if (tid >= 0) {
            ret = (int64_t)tid;
            child_pml4 = PHYS_NULL; // ownership moved to the new task
        }
    }

    if (child_pml4 != PHYS_NULL) {
        // CR3-neutral destroy: exec_load_image already restored the
        // caller's CR3; destroy_task_as would leave kernel CR3 behind
        // for the iretq below (pre-existing hazard, fixed here).
        vmm_destroy_address_space(child_pml4, 1);
    }
    return ret;
}

int sys_proc_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    (void)st;
    uint64_t syscall_num = r->rax;

    if (syscall_num == 25) { // sys_load_elf
        // FIX Tahap 2 (UAF): copy filename SEBELUM AS lama dihancurkan dan
        // CR3 pindah ke AS baru — sebelumnya string user di-deref SETELAH
        // switch, membaca alamat dari AS yang sudah tidak ada.
        char kfname[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kfname, r->rbx, sizeof(kfname)) < 0) {
            r->rax = 0;
            return 1;
        }

        // Flush KEDUA buffer sebelum app baru jalan (Phase 5B: queue per-task)
        flush_event_queue(smp_current_task_id());
        flush_kbd_buffer();

        // --- PER-PROCESS ISOLATION ---
        // Create a fresh address space for the new user app.
        // current_pml4 ALWAYS stays as kernel PML4. Target PML4 untuk ELF
        // load diteruskan eksplisit ke elf_load_file (FIX_002), lalu CR3
        // di-switch ke AS baru.
        phys_addr_t new_pml4 = vmm_create_address_space();
        if (new_pml4 != PHYS_NULL) {
            // Clean old user pages if this task had a previous AS
            task_t *self = syscall_current_task();
            if (self && self->pml4_phys != 0) {
                vmm_destroy_task_as(self->pml4_phys);
            }

            if (self) {
                // Tahap 3: region heap user mati bersama AS lama — buang
                // metadata + mulai brk segar untuk AS baru.
                uheap_reset(self);
                self->pml4_phys = new_pml4;
                self->cookie = as_cookie_next();
            }

            // Switch CR3 to the user PML4 BEFORE loading. elf_load_file copies
            // segment bytes directly to user virtual addresses (e.g. 0x4000000)
            // via memcpy, which the CPU translates through the *current* CR3.
            // The freshly-allocated pages are mapped into new_pml4, so CR3 must
            // already point there or the copy faults on an unmapped address.
            // Kernel higher-half (code/stack/heap/HHDM) is cloned into new_pml4,
            // so kernel execution continues safely after the switch.
            vmm_switch_pml4(new_pml4);
        }

        // Tahap 3: stack ter-map di user range AS baru — bebas bersama AS,
        // tidak ada lagi tracking kfree.
        uint64_t new_stack_top = 0;
        *ret = elf_load_file(kfname, &new_stack_top, new_pml4);
    }
    else if (syscall_num == 33) { // sys_exec — load & jalankan ELF baru, replace current app
        // PENTING: copy filename ke kernel stack DULU sebelum unmap!
        // Tahap 2: lewat strncpy_from_user (tervalidasi); gagal → keluar
        // SEBELUM window/AS caller disentuh.
        char kfname[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kfname, r->rbx, sizeof(kfname)) < 0) {
            r->rax = 0;
            return 1;
        }
#ifdef HEAP_WATCH_DEBUG
        {
            serial_print("[EXEC] kfname=[");
            serial_print(kfname);
            serial_print("] rbx=");
            serial_print_hex(r->rbx);
            serial_print(" cs=");
            serial_print_hex(r->cs);
            serial_print("\n");
        }
#endif

        // 0. Destroy KWM windows milik TASK INI saja (FIX_004) — compositor
        //    tetap aman tanpa menghancurkan window milik task lain.
        kwm_destroy_windows_of(smp_current_task_id());

        // 1. Destroy current address space and switch back to kernel PML4
        {
            task_t *self = syscall_current_task();

            if (self && self->pml4_phys != 0) {
                vmm_destroy_task_as(self->pml4_phys);
                self->pml4_phys = 0;
            }
            // Tahap 3: stack & heap user hidup di AS — frame ikut bebas di
            // vmm_destroy_task_as; sisa metadata heap user dibuang di sini.
            uheap_reset(self);
            // pml4_phys == 0: task memakai boot/kernel AS. User range PML4 boot
            // milik Limine — membebaskannya mencemari free list PMM dengan
            // halaman reserved/ROM <72MB (akar BOSD heap corruption).
        }

        // 1b. Flush input buffers so new app doesn't inherit old keystrokes
        //     (Phase 5B: event queue per-task — flush milik task ini saja)
        {
            flush_event_queue(smp_current_task_id());
            flush_kbd_buffer();
        }

        // 2. Create fresh AS for the new app being exec'd
        phys_addr_t new_pml4 = PHYS_NULL;
        {
            new_pml4 = vmm_create_address_space();
            if (new_pml4 != PHYS_NULL) {
                task_t *self = syscall_current_task();
                if (self) {
                    self->pml4_phys = new_pml4;
                    self->cookie = as_cookie_next();
                }

                // Switch CR3 to the user PML4 BEFORE loading — elf_load_file
                // copies segment bytes to user virtual addresses via memcpy,
                // translated through the current CR3. The target pages live in
                // new_pml4, so CR3 must point there or the copy page-faults.
                vmm_switch_pml4(new_pml4);
            }
        }

        // 3. Load ELF baru ke slot 0x4000000 — target PML4 eksplisit (FIX_002)
        // Tahap 3: stack di user range AS baru, dibebaskan bersama AS.
        uint64_t new_stack_top = 0;
        uint64_t entry = elf_load_file(kfname, &new_stack_top, new_pml4);

        // 3. Set RIP & RSP untuk IRETQ
        if (entry != 0) {
            r->rip = entry;
            // New app runs on its own 256KB stack (deep decode chains overflow
            // the shared shell stack). Fallback to shell RSP if alloc failed.
            r->rsp = (new_stack_top != 0) ? new_stack_top : g_shell_return_rsp;
            // FIX_005 Tahap 1: masuk CPL 3 — iretq memuat segmen user.
            // Kernel tetap bisa dijangkau via int 0x80 (gate DPL=3 + TSS.RSP0).
            r->cs = 0x1B;  // user code (GDT[3] | RPL3)
            r->ss = 0x23;  // user data (GDT[4] | RPL3)
#ifdef HEAP_WATCH_DEBUG
            serial_print("[EXEC] ring3 cs=");
            serial_print_hex(r->cs);
            serial_print(" ss=");
            serial_print_hex(r->ss);
            serial_print(" rip=");
            serial_print_hex(r->rip);
            serial_print(" rsp=");
            serial_print_hex(r->rsp);
            serial_print("\n");
#endif
            *ret = 1;
        } else {
            *ret = 0;
        }
    }
    else if (syscall_num == 34) { // sys_exit(code) — P0 Phase 2: RBX = exit status
        // Exit status vs syscall failure are DISTINCT: a syscall returning -1
        // never implies process exit -1. Only this path records exit_code.
        // TASK_KIND_SPAWNED (sys_spawn child): terminate via proc_exit —
        // zombie iff a live parent can waitpid, else DEAD immediately.
        // TASK_KIND_KERNEL (exec-chain shell): legacy longjmp back to the
        // shell loop (task reused, no zombie). Code ignored on that path.
        int code = (int32_t)r->rbx;   // old void callers now pass 0 explicitly
        {
            task_t *self = syscall_current_task();
            if (self && self->kind == TASK_KIND_SPAWNED) {
                proc_exit(code);  // noreturn
            }
        }

        // Kernel exec-chain path: destroy this task's windows only (FIX_004).
        // (Spawned path already did this inside proc_exit.)
        {
            kwm_destroy_windows_of(smp_current_task_id());
        }

        // Destroy address space and switch back to kernel PML4
        {
            task_t *self = syscall_current_task();

            // Tahap 3: buang metadata heap user — frame region & stack app
            // ikut bebas saat AS dihancurkan di bawah.
            uheap_reset(self);

            if (self && self->pml4_phys != 0) {
                vmm_destroy_task_as(self->pml4_phys);
                self->pml4_phys = 0;
            }
            // pml4_phys == 0: task memakai boot/kernel AS. User range PML4 boot
            // milik Limine — membebaskannya mencemari free list PMM dengan
            // halaman reserved/ROM <72MB (akar BOSD heap corruption).
        }

        // Longjmp kembali ke shell: reset RSP dan jump ke user_shell()
        // Ini BYPASS iretq sepenuhnya — langsung ke shell command loop.
        // Aman karena int 0x80 = software interrupt (tidak perlu EOI).
        uint64_t safe_rsp = g_shell_return_rsp;
        if (safe_rsp == 0) {
            // Fallback: jika belum pernah launch app dari shell, halt
            for(;;) __asm__ volatile("hlt");
        }
        // Reset stack dan jump langsung ke shell loop.
        // Tidak pakai CALL (yang push return addr dan grow stack).
        // Pakai JMP → shell berjalan di stack level yang sama.
        __asm__ volatile(
            "mov %0, %%rsp\n"
            "xor %%rbp, %%rbp\n"
            "sti\n"                // Re-enable interrupts! (int 0x80 disabled mereka)
            "jmp *%1\n"
            : : "r"(safe_rsp), "r"((uint64_t)user_shell)
            : "memory"
        );
        __builtin_unreachable();
    }
    else if (syscall_num == 57) { // sys_spawn — Phase 5A: ELF sebagai task ring-3 BARU
        // Copy-in path SEBELUM apa pun (pola boundary Tahap 2).
        // P0 Phase 2: argv[0] = basename(path), argc = 1. Full argv via 68.
        char kfname[UC_MAX_FNAME];
        if (strncpy_from_user(uc, kfname, r->rbx, sizeof(kfname)) < 0) {
            r->rax = (uint64_t)-1;
            return 1;
        }

        char kargv[1][PROC_MAX_ARG_LEN];
        {
            proc_basename(kfname, kargv[0], sizeof(kargv[0]));
        }
        *ret = (uint64_t)spawn_common(kfname, 1, kargv, NULL);
    }
    else if (syscall_num == 68) { // sys_spawn_argv(path, argc, argv) — P0 Phase 2
        // RBX=path, RCX=argc, RDX=argv (user char**). Bounded, fail safe.
        char kfname[UC_MAX_FNAME];
        int argc = (int)r->rcx;
        if (strncpy_from_user(uc, kfname, r->rbx, sizeof(kfname)) < 0 ||
            !proc_argc_valid(argc)) {
            r->rax = (uint64_t)-1;
            return 1;
        }
        // 1KB on the kernel stack (16KB task stack): no SMP sharing, no alloc fail.
        char kargv[PROC_MAX_ARGC][PROC_MAX_ARG_LEN];
        {
            if (proc_copy_in_argv(uc, r->rdx, argc, kargv, NULL) != 0) {
                r->rax = (uint64_t)-1;
                return 1;
            }
        }
        *ret = (uint64_t)spawn_common(kfname, argc, kargv, NULL);
    }
    else if (syscall_num == SYS_SPAWN_REDIR) { // sys_spawn_redir — P0 Phase 5
        // Like 68, plus RSI=spec* (user spawn_stdio_t, 12 bytes): the
        // caller's fds to install as the child's 0/1/2 (-1 = fresh TTY).
        // Explicit per-fd inheritance for shell redirection/pipelines —
        // never a whole-table copy, never another task's table.
        char kfname[UC_MAX_FNAME];
        int argc = (int)r->rcx;
        if (strncpy_from_user(uc, kfname, r->rbx, sizeof(kfname)) < 0 ||
            !proc_argc_valid(argc)) {
            r->rax = (uint64_t)-1;
            return 1;
        }
        char kargv[PROC_MAX_ARGC][PROC_MAX_ARG_LEN];
        {
            if (proc_copy_in_argv(uc, r->rdx, argc, kargv, NULL) != 0) {
                r->rax = (uint64_t)-1;
                return 1;
            }
        }
        spawn_stdio_t kspec;
        if (copy_from_user(uc, &kspec, r->rsi, sizeof(kspec)) != 0 ||
            !spawn_stdio_valid(kspec.fd0) ||
            !spawn_stdio_valid(kspec.fd1) ||
            !spawn_stdio_valid(kspec.fd2)) {
            r->rax = (uint64_t)-1;
            return 1;
        }
        int inherit_fds[3] = { kspec.fd0, kspec.fd1, kspec.fd2 };
        *ret = (uint64_t)spawn_common(kfname, argc, kargv, inherit_fds);
    }
    else if (syscall_num == SYS_EXECVE) { // sys_execve(path, argc, argv) — P0 Phase 6C
        // Atomic in-place image replace of the CALLING task: the new image
        // is fully built in a separate AS first; only then is the task
        // switched (AS + heap + argv + trap frame) and the old AS destroyed.
        // Any failure returns -1 with the old image 100% runnable.
        // Ring-3 user tasks only (kernel tasks have no user image to
        // replace; forging user CS/SS from a kernel trap is refused).
        // PID/PPID/creds/FDs/windows all survive (no CLOEXEC in this arch).
        char kfname[UC_MAX_FNAME];
        int argc = (int)r->rcx;
        if (strncpy_from_user(uc, kfname, r->rbx, sizeof(kfname)) < 0 ||
            !proc_argc_valid(argc)) {
            r->rax = (uint64_t)-1;
            return 1;
        }
        char kargv[PROC_MAX_ARGC][PROC_MAX_ARG_LEN];
        {
            if (proc_copy_in_argv(uc, r->rdx, argc, kargv, NULL) != 0) {
                r->rax = (uint64_t)-1;
                return 1;
            }
        }
        task_t* self = syscall_current_task();
        if (!self || ((r->cs & 3) != 3) || self->pml4_phys == PHYS_NULL) {
            r->rax = (uint64_t)-1;
            return 1;
        }
        phys_addr_t new_as = vmm_create_address_space();
        if (new_as == PHYS_NULL) {
            r->rax = (uint64_t)-1;
            return 1;
        }
        uint64_t new_stack = 0, new_argv = 0;
        uint64_t entry = exec_load_image(self, kfname, argc, kargv, new_as,
                                         &new_stack, &new_argv);
        if (entry == 0 || new_stack == 0) {
            // CR3-neutral destroy: exec_load_image already restored CR3
            // to the OLD as (still current) — destroy_task_as would
            // switch to kernel CR3 and strand the iretq below in the
            // wrong address space. Old image untouched either way.
            vmm_destroy_address_space(new_as, 1);
            r->rax = (uint64_t)-1;
            return 1;
        }
        // COMMIT: everything validated. Adopt the new AS on this CPU (the
        // iretq below must translate through it), publish it on the task,
        // refresh image-bound metadata, drop the old heap metadata (its
        // pages die with the old AS), then destroy the old AS (HHDM-based,
        // CR3-independent — never touches the now-current tables).
        phys_addr_t old_as = self->pml4_phys;
        vmm_switch_pml4(new_as);
        self->pml4_phys = new_as;
        self->cookie = as_cookie_next();
        self->argc = (int32_t)argc;
        {
            proc_basename(kfname, self->name, sizeof(self->name));
        }
        uheap_reset(self);
        {
            flush_event_queue(smp_current_task_id());   // own queue only
        }
        // NOTE: KWM windows are KEPT — the task persists (unlike legacy 33,
        // which tears down the caller's windows for the exec-chain shell).
        vmm_destroy_address_space(old_as, 1);
        // Fresh-image startup frame (NOT a fork resume): ELF entry, new
        // stack/argv, zeroed GP regs. cs/ss already user (gated above).
        r->rip = entry;
        r->rsp = new_stack;
        r->rdi = (uint64_t)argc;
        r->rsi = new_argv;
        r->rax = 0; r->rbx = 0; r->rcx = 0; r->rdx = 0; r->rbp = 0;
        r->r8 = 0; r->r9 = 0; r->r10 = 0; r->r11 = 0;
        r->r12 = 0; r->r13 = 0; r->r14 = 0; r->r15 = 0;
        *ret = 0;
    }
    else if (syscall_num == SYS_FORK) { // sys_fork() — P0 Phase 6B
        // Ring-3 only: forging a child trap frame from a kernel trap
        // frame would iretq with kernel CS/SS. Kernel tasks (no user
        // AS) cannot fork either. Parent returns via r->rax below;
        // the child resumes after this trap with rax=0 (see task_fork).
        int child = -1;
        if (((r->cs & 3) == 3)) {
            task_t* self = syscall_current_task();
            if (self && self->pml4_phys != PHYS_NULL) {
                child = task_fork(smp_current_task_id(), r);
            }
        }
        *ret = (uint64_t)(int64_t)child;   // child pid, or -1
    }
    else if (syscall_num == 69) { // sys_waitpid(pid, status*, options) — P0 Phase 2
        // RBX=pid (atau -1 = any child), RCX=status* (user int*, 0=ignore),
        // RDX=options (must 0; no WNOHANG yet). Parent-only: unrelated pid -> -1.
        // Blocks (no polling) via proc_wq; child exit wakes us cross-CPU.
        int32_t pid = (int32_t)r->rbx;
        uint64_t u_status = r->rcx;
        int options = (int)r->rdx;
        int32_t kstatus = 0;
        int got = -1;
        if (options == 0 && (u_status == 0 || user_range_ok(uc, u_status, 4))) {
            got = proc_waitpid(pid, u_status ? &kstatus : NULL, 0);
            if (got >= 0 && u_status) {
                if (copy_to_user(uc, u_status, &kstatus, 4) != 0) got = -1;
            }
        }
        *ret = (uint64_t)(int64_t)got;
    }
    else if (syscall_num == 70) { // sys_getpid — P0 Phase 2: task id (-1 idle)
        *ret = (uint64_t)(int64_t)proc_getpid();
    }
    else if (syscall_num == 71) { // sys_getppid — P0 Phase 2: parent or NO_PARENT
        *ret = (uint64_t)(int64_t)proc_getppid();
    }
    else if (syscall_num == 72) { // sys_proc_list(buf, max) — P0 Phase 2
        // RBX=buf (user proc_info_t*), RCX=max entries. Returns count or -1.
        // Read-only snapshot for Task Manager; userspace cannot mutate tasks.
        int maxn = (int)r->rcx;
        if (maxn < 1) maxn = 1;
        if (maxn > MAX_TASKS) maxn = MAX_TASKS;
        uint64_t bytes = (uint64_t)maxn * sizeof(proc_info_t);
        int count = -1;
        if (user_range_ok(uc, r->rbx, bytes)) {
            proc_info_t* bounce = (proc_info_t*)kmalloc((uint32_t)bytes);
            if (bounce) {
                count = proc_fill_list(bounce, maxn);
                if (count > 0 &&
                    copy_to_user(uc, r->rbx, bounce,
                                 (uint64_t)count * sizeof(proc_info_t)) != 0) {
                    count = -1;
                }
                kfree(bounce);
            }
        }
        *ret = (uint64_t)(int64_t)count;
    }
    // P0 Phase 3 — sys_kill(pid): request termination of pid.
    // RBX=pid. Policy enforced in kernel (proc_kill): normal user may kill
    // own child (parent_id-based, never UID-based); root may kill any
    // spawned task; PID 0 / kernel tasks / invalid / already-exited -> -1.
    // READY targets die synchronously; BLOCKED/SLEEPING/RUNNING targets
    // converge on proc_exit_kill() and are reaped via waitpid() with
    // PROC_KILL_EXIT_CODE / PROC_EXIT_KILLED.
    else if (syscall_num == 73) { // sys_kill
        *ret = (uint64_t)(int64_t)proc_kill((int32_t)r->rbx);
    }

    return 0;
}
