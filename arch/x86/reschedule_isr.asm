bits 64
%include "arch/x86/isr_macro.inc"

section .text
global reschedule_isr_stub
extern lapic_reschedule_handler

; ============================================================
; reschedule_isr_stub — IPI reschedule handler (vector 0xFD)
;
; Called when another CPU sends an IPI to wake this CPU from
; idle and force a schedule pass.
; ============================================================
reschedule_isr_stub:
    KZ_ENTRY_PROLOGUE       ; ARCH-1: cld + clac
    push 0                  ; error_code = 0
    push 253                ; int_num = 0xFD (LAPIC_RESCHEDULE_VECTOR)
    PUSHA64

    mov rdi, rsp            ; arg1 = registers_t* current_regs
    call lapic_reschedule_handler
    KZ_VALIDATE_RSP 253     ; ARCH-8: validasi RSP sebelum dipakai

    POPA64
    add rsp, 16             ; skip int_num + error_code
    iretq
