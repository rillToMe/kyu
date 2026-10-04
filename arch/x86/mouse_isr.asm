bits 64
%include "arch/x86/isr_macro.inc"
section .text
global mouse_isr_stub
extern mouse_handler

mouse_isr_stub:
    KZ_ENTRY_PROLOGUE   ; ARCH-1: cld + clac
    push 0
    push 44         ; IRQ 12 (Slave PIC → INT 0x2C = 44)
    PUSHA64
    call mouse_handler  ; mouse_handler() tidak punya parameter
    POPA64
    add rsp, 16
    iretq
