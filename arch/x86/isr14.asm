bits 64
%include "arch/x86/isr_macro.inc"
section .text
global isr14_stub
extern page_fault_handler

isr14_stub:
    KZ_ENTRY_PROLOGUE   ; ARCH-1: cld + clac
    ; CPU otomatis push error code
    push 14         ; Push int_num
    PUSHA64
    
    mov rdi, rsp    ; Arg 1 (RDI) = struct registers*
    mov rsi, cr2    ; Arg 2 (RSI) = fault_addr (CR2)
    
    call page_fault_handler
    
    POPA64
    add rsp, 16
    iretq