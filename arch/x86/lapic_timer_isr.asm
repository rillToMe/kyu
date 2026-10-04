bits 64
%include "arch/x86/isr_macro.inc"

section .text
global lapic_timer_isr_stub
extern lapic_timer_handler

lapic_timer_isr_stub:
    KZ_ENTRY_PROLOGUE   ; ARCH-1: cld + clac
    push 0
    push 240
    PUSHA64

    mov rdi, rsp
    call lapic_timer_handler
    KZ_VALIDATE_RSP 240 ; ARCH-8: validasi RSP sebelum dipakai

    POPA64
    add rsp, 16
    iretq
