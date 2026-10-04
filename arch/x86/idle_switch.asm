bits 64
; ============================================================
; idle_switch.asm — Pindah ke idle stack permanen per-CPU (FIX_001)
;
; Pergantian RSP tidak bisa dilakukan aman dari C, jadi trampolinnya
; di sini. Kedua entry point tidak pernah kembali (noreturn).
; ============================================================
section .text

extern scheduler_idle_loop
extern task_exit_finish_on_idle

global task_switch_to_idle_stack
global task_exit_via_idle

; void task_switch_to_idle_stack(uint64_t stack_top) — noreturn
; Dipakai smp_ap_main: tinggalkan stack awal dari Limine, masuk idle loop
; di idle stack permanen CPU ini.
task_switch_to_idle_stack:
    mov     rsp, rdi
    xor     rbp, rbp            ; putus frame chain ke stack lama
    jmp     scheduler_idle_loop

; void task_exit_via_idle(uint64_t stack_top, void* old_stack_base) — noreturn
; Dipakai task_exit: pindah DULU ke idle stack, baru free stack task DEAD.
; Stack lama aman di-free setelah mov rsp — reaper di create_task tidak
; pernah menyentuhnya (stack_base slot sudah di-zero-kan di bawah
; scheduler_lock oleh task_exit).
task_exit_via_idle:
    mov     rsp, rdi
    xor     rbp, rbp
    mov     rdi, rsi            ; arg1 finish = old_stack_base
    jmp     task_exit_finish_on_idle
