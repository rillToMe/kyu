#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>    // NULL (dipakai handler subsystem)
#include "task.h"      // registers_t, task_t
#include "usercopy.h"  // ucopy_ctx_t

// ============================================================
// kernel/syscall/ — shared dispatcher API (pure structural split
// dari kernel/syscall.c; ABI/behavior tidak berubah).
//
// Protokol return handler subsystem:
//   0 = normal: dispatcher lanjut ke kill-pending post-check lalu
//       r->rax = *ret.
//   1 = early-return: handler sudah mengisi r->rax sendiri;
//       dispatcher HARUS langsung return (melewati post-check dan
//       final r->rax) — replika `r->rax = X; return;` inline lama.
// Path noreturn (sys_exit, proc_exit_kill) tidak pernah kembali.
// ============================================================

task_t* syscall_current_task(void);

// Counter: setiap sys_yield menambahkannya; timer membaca + mereset
// tiap tick untuk CPU idle tracking. Definisi di sys_system.c; deklarasi
// kanonis di sini (konsumen timer.c/kernel_userlib.c memakai extern sendiri
// yang tidak diubah phase ini).
extern volatile uint32_t yield_counter;

// Diagnostik panic g_last_syscall_* TIDAK dideklarasikan di sini: definisi di
// syscall.c, konsumen (kernel/panic) memakai deklarasinya sendiri di
// panic_internal.h — satu-satunya TU lain. Tidak ada yang perlu header ini.

int sys_proc_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st);
int sys_mem_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st);
int sys_fs_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st);
int sys_vfs_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st);
int sys_kwm_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st);
int sys_net_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st);
int sys_gpu_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st);
int sys_system_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st);
int sys_misc_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st);

#endif // SYSCALL_H
