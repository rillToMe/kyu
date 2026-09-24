#include "syscall.h"
#include <stdint.h>
#include "task.h"
#include "usercopy.h"
#include "proc.h"          // SYS_SPAWN_REDIR / SYS_EXECVE / SYS_FORK / SYS_WAITPID /
                           // SYS_GETPID / SYS_GETPPID / SYS_PROC_LIST / SYS_KILL
#include "vfs.h"           // SYS_DUP / SYS_DUP2 / SYS_PIPE / SYS_READDIR /
                           // SYS_RENAME / SYS_STAT
#include "crash_notice.h"  // SYS_CRASH_NOTICE
#include "kwm_abi.h"       // SYS_WALLPAPER_RELOAD

// registers_t is provided by task.h — must match PUSHA64 in isr_macro.inc

// ========================================================
// HANDLER SYSCALL 64-BIT
// (Dipanggil oleh isr128_stub saat aplikasi melempar int 0x80)
//
// Dispatcher murni: common setup (diagnostik panic, ucopy context,
// kill-pending pre-check) → route ke handler subsystem di sys_*_handle
// → kill-pending post-check → r->rax. Tidak ada logika subsystem di sini.
// Nomor syscall, ABI register, dan semantik return tidak berubah.
// ========================================================

task_t* syscall_current_task(void) {
    int task_id = smp_current_task_id();
    if (task_id < 0 || task_id >= task_count) return NULL;
    return &tasks[task_id];
}

// Diagnostik panic (kernel/panic/): konteks syscall TERAKHIR. Best-effort,
// non-atomic, tanpa lock — tujuannya hanya memberi petunjuk "app sedang apa"
// saat kernel mati. Tidak dipakai untuk logika apa pun.
volatile uint64_t g_last_syscall_num  = 0;
volatile int32_t  g_last_syscall_task = -1;
volatile uint64_t g_last_syscall_arg0 = 0;
volatile uint64_t g_last_syscall_arg1 = 0;
volatile uint64_t g_last_syscall_arg2 = 0;

void syscall_handler(registers_t *r) {
    task_t* syscall_task = syscall_current_task();

    // Nomor Syscall selalu ada di RAX
    uint64_t syscall_num = r->rax;
    uint64_t ret_val = 0; // Default return

    // Catat konteks untuk BSOD (lihat kernel/panic/panic.c).
    g_last_syscall_num  = syscall_num;
    g_last_syscall_task = syscall_task ? (int32_t)syscall_task->id : -1;
    g_last_syscall_arg0 = r->rdi;
    g_last_syscall_arg1 = r->rsi;
    g_last_syscall_arg2 = r->rdx;

    // FIX_005 Tahap 2: konteks boundary copy — pointer dari ring 3
    // divalidasi & di-copy; caller ring 0 (shell/login kernel via int 0x80)
    // lewat jalur bypass (pointer kernel sah).
    ucopy_ctx_t uc;
    ucopy_ctx_init(&uc, r);

    // P0 Phase 3: kill observed on trap entry. A RUNNING target flagged by
    // proc_kill converges here (its next syscall) without running further
    // user code. Kernel tasks are never flagged (kind gate in proc_kill).
    if (syscall_task && syscall_task->kill_pending) {
        proc_exit_kill();   // noreturn
    }

    // --- Pemetaan Argumen Standar Kyuzen OS 64-bit ---
    // RAX = Nomor Syscall
    // RBX = Argumen 1
    // RCX = Argumen 2
    // RDX = Argumen 3
    // RSI = Argumen 4
    // RDI = Argumen 5

    // Route per subsystem (if/else dipertahankan dari dispatcher lama —
    // belum syscall_table; itu phase terpisah). Handler mengembalikan 1
    // bila ia sudah mengisi r->rax + minta early-return (replika
    // `r->rax = X; return;` inline lama: post-check dilewati).
    int done = 0;
    if (syscall_num == 25 || syscall_num == 33 || syscall_num == 34 ||
        syscall_num == 57 || syscall_num == 68 ||
        syscall_num == 69 || syscall_num == 70 || syscall_num == 71 ||
        syscall_num == 72 || syscall_num == 73 ||
        syscall_num == SYS_SPAWN_REDIR || syscall_num == SYS_EXECVE ||
        syscall_num == SYS_FORK) {
        done = sys_proc_handle(r, &uc, &ret_val, syscall_task);
    }
    else if (syscall_num == 9 || syscall_num == 10 || syscall_num == 19) {
        done = sys_mem_handle(r, &uc, &ret_val, syscall_task);
    }
    else if (syscall_num == 5 || syscall_num == 6 || syscall_num == 7 ||
             syscall_num == 8 || syscall_num == 11 || syscall_num == 12 ||
             syscall_num == 13 || syscall_num == 18 || syscall_num == 24 ||
             syscall_num == 64 || syscall_num == SYS_RENAME ||
             syscall_num == SYS_STAT) {
        done = sys_fs_handle(r, &uc, &ret_val, syscall_task);
    }
    else if (syscall_num == 47 || syscall_num == 48 || syscall_num == 49 ||
             syscall_num == 50 || syscall_num == 51 ||
             syscall_num == SYS_DUP || syscall_num == SYS_DUP2 ||
             syscall_num == SYS_READDIR || syscall_num == SYS_PIPE) {
        done = sys_vfs_handle(r, &uc, &ret_val, syscall_task);
    }
    else if (syscall_num == 22 || syscall_num == 23 || syscall_num == 26 ||
             syscall_num == 29 || syscall_num == 30 || syscall_num == 31 ||
             syscall_num == 32 || syscall_num == 58 || syscall_num == 59 ||
             syscall_num == 60 || syscall_num == 61 || syscall_num == 62 ||
             syscall_num == 63 || syscall_num == 66 || syscall_num == 67 ||
             syscall_num == SYS_WALLPAPER_RELOAD) {
        done = sys_kwm_handle(r, &uc, &ret_val, syscall_task);
    }
    else if (syscall_num == 41 || syscall_num == 52 || syscall_num == 53 ||
             syscall_num == 54 || syscall_num == 55 || syscall_num == 56) {
        done = sys_net_handle(r, &uc, &ret_val, syscall_task);
    }
    else if (syscall_num == 65) {
        done = sys_gpu_handle(r, &uc, &ret_val, syscall_task);
    }
    else if (syscall_num == 4 || syscall_num == 14 || syscall_num == 15 ||
             syscall_num == 16 || syscall_num == 17 || syscall_num == 20 ||
             syscall_num == 35 || syscall_num == 36 || syscall_num == 37 ||
             syscall_num == 38 || syscall_num == 39 || syscall_num == 46) {
        done = sys_system_handle(r, &uc, &ret_val, syscall_task);
    }
    else if (syscall_num == 1 || syscall_num == 2 || syscall_num == 3 ||
             syscall_num == 27 || syscall_num == 28 || syscall_num == 42 ||
             syscall_num == 43 || syscall_num == 44 || syscall_num == 45 ||
             syscall_num == SYS_CRASH_NOTICE) {
        done = sys_misc_handle(r, &uc, &ret_val, syscall_task);
    }
    else if (syscall_num == 21) {
        // Reserved/Unused
    }
    if (done) return;

    // P0 Phase 3: kill observed on trap exit. Covers "flag set while
    // blocked inside this syscall": woken by a non-kill event, the task
    // must not return to user code with termination outstanding.
    if (syscall_task && syscall_task->kill_pending) {
        proc_exit_kill();   // noreturn
    }

    // SIMPAN RETURN VALUE KE RAX (Penting untuk aplikasi Ring 3!)
    r->rax = ret_val;
}
