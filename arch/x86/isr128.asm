bits 64
%include "arch/x86/isr_macro.inc"
section .text
global isr128_stub
extern syscall_handler

isr128_stub:
    KZ_ENTRY_PROLOGUE   ; ARCH-1: cld + clac
    push 0          ; dummy error code
    push 128        ; int_num
    PUSHA64
    
    mov rdi, rsp    ; Arg 1: struct registers*
    call syscall_handler

    ; FIX_005 Tahap 2: JANGAN timpa slot RAX dengan sisa register RAX dari
    ; fungsi C — syscall_handler bertipe void dan menulis return value ke
    ; frame (r->rax) di SEMUA jalur. Trik lama ([rsp+112] = RAX) hanya
    ; kebetulan benar dan pecah begitu ada call lain sebelum return.

    POPA64          ; Kembalikan semua; RAX = r->rax yang ditulis handler
    add rsp, 16
    iretq