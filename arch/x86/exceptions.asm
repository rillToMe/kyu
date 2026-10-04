bits 64
; ============================================================
; arch/x86/exceptions.asm — Gerbang exception x86-64, Kyuzen OS
;
; ARCH-3: SEMUA 256 vektor kini punya stub.
;
; Sebelumnya hanya 0..8 dan 10..14 yang punya stub. Vektor lain (mis. 18 #MC,
; 16 #MF, 19 #XM, dan 255 = spurious LAPIC yang MEMANG disetel di LAPIC SVR)
; jatuh ke IDT entry nol → #GP → #DF → triple fault / reset tanpa BSOD.
; Sekarang setiap vektor yang tidak punya handler khusus memakai stub default
; yang melaporkan vektor + error code dengan jelas.
;
; KONVENSI FRAME (dipakai seluruh ISR, lihat isr_macro.inc):
;   CPU auto-push: SS, RSP, RFLAGS, CS, RIP        (5 qword)
;   [exception ber-error-code: CPU juga push error_code]
;   Stub push:     error_code (0 bila CPU tidak push), int_num
;   PUSHA64:       15 GP register
;
; Setelah PUSHA64, RSP menunjuk ke r15 = pointer ke registers_t.
;
; ARCH-1: setiap stub diawali KZ_ENTRY_PROLOGUE (cld + clac).
; ============================================================

%include "arch/x86/isr_macro.inc"

section .text
extern exception_handler
extern default_interrupt_handler

; ------------------------------------------------------------
; Macro stub.
;
; CATATAN PENTING soal error code: hanya vektor yang tercantum di Intel SDM
; Vol.3 Table 6-1 yang mem-push error code. Memakai ISR_ERRCODE pada vektor
; yang TIDAK mem-push error code akan menggeser seluruh frame 8 byte dan
; membuat `registers_t` salah baca → crash yang membingungkan. Daftar di
; bawah sudah dicocokkan dengan tabel SDM.
; ------------------------------------------------------------

%macro ISR_NOERRCODE 1
global isr%1_stub
isr%1_stub:
    KZ_ENTRY_PROLOGUE       ; ARCH-1: cld + clac (WAJIB paling awal)
    push 0                  ; dummy error_code agar frame seragam
    push %1                 ; int_num
    PUSHA64
    mov rdi, rsp            ; arg1 = registers_t*
    call exception_handler
    POPA64
    add rsp, 16             ; buang int_num + error_code
    iretq
%endmacro

%macro ISR_ERRCODE 1
global isr%1_stub
isr%1_stub:
    KZ_ENTRY_PROLOGUE       ; ARCH-1: cld + clac (WAJIB paling awal)
    ; CPU sudah mem-push error_code
    push %1                 ; int_num
    PUSHA64
    mov rdi, rsp            ; arg1 = registers_t*
    call exception_handler
    POPA64
    add rsp, 16             ; buang int_num + error_code
    iretq
%endmacro

; --- Vektor 0..31 -----------------------------------------------------------
ISR_NOERRCODE 0             ; #DE Divide Error
ISR_NOERRCODE 1             ; #DB Debug
ISR_NOERRCODE 2             ; NMI             (IST di IDT — ARCH-2)
ISR_NOERRCODE 3             ; #BP Breakpoint  (DPL=3 di IDT)
ISR_NOERRCODE 4             ; #OF Overflow    (DPL=3 di IDT)
ISR_NOERRCODE 5             ; #BR BOUND Range (DPL=3 di IDT)
ISR_NOERRCODE 6             ; #UD Invalid Opcode (DPL=3 di IDT)
ISR_NOERRCODE 7             ; #NM Device Not Available
ISR_ERRCODE   8             ; #DF Double Fault (IST di IDT — ARCH-2)
ISR_NOERRCODE 9             ; legacy coprocessor segment overrun (tanpa error code)
ISR_ERRCODE   10            ; #TS Invalid TSS
ISR_ERRCODE   11            ; #NP Segment Not Present
ISR_ERRCODE   12            ; #SS Stack-Segment Fault
ISR_ERRCODE   13            ; #GP General Protection
; 14 = #PF punya file sendiri (isr14.asm) karena butuh CR2 sebagai arg2
ISR_NOERRCODE 15            ; reserved (tanpa error code)
ISR_NOERRCODE 16            ; #MF x87 FPU Floating-Point Error (tanpa error code)
ISR_ERRCODE   17            ; #AC Alignment Check
ISR_NOERRCODE 18            ; #MC Machine Check (IST di IDT — ARCH-2)
ISR_NOERRCODE 19            ; #XM SIMD Floating-Point
ISR_NOERRCODE 20            ; #VE Virtualization Exception
ISR_ERRCODE   21            ; #CP Control Protection
ISR_NOERRCODE 22            ; reserved
ISR_NOERRCODE 23            ; reserved
ISR_NOERRCODE 24            ; reserved
ISR_NOERRCODE 25            ; reserved
ISR_NOERRCODE 26            ; reserved
ISR_NOERRCODE 27            ; reserved
ISR_NOERRCODE 28            ; #HV Hypervisor Injection (tanpa error code)
ISR_ERRCODE   29            ; #VC VMM Communication
ISR_ERRCODE   30            ; #SX Security Exception
ISR_NOERRCODE 31            ; reserved

; ------------------------------------------------------------
; Vektor 32..255 — stub default.
;
; Termasuk PIC IRQ (32..47 setelah remap) yang tidak punya driver: tanpa stub,
; IRQ yang tidak dimask akan #GP → panic. Sekarang IRQ tak terduga dilaporkan
; (dibatasi) lalu di-EOI supaya sistem tetap hidup.
; ------------------------------------------------------------

%macro ISR_DEFAULT 1
global isr%1_stub
isr%1_stub:
    KZ_ENTRY_PROLOGUE       ; ARCH-1: cld + clac (WAJIB paling awal)
    push 0                  ; dummy error_code (vektor 32..255 tidak punya)
    push %1                 ; int_num
    PUSHA64
    mov rdi, rsp            ; arg1 = registers_t*
    call default_interrupt_handler
    POPA64
    add rsp, 16
    iretq
%endmacro

%assign vec 32
%rep 96                     ; vektor 32..127
    ISR_DEFAULT vec
%assign vec vec+1
%endrep

; 128 = syscall — isr128.asm. JANGAN emit stub default di sini: simbol
; `isr128_stub` sudah didefinisikan di file itu dan akan bentrok saat link.

%assign vec 129
%rep 111                    ; vektor 129..239
    ISR_DEFAULT vec
%assign vec vec+1
%endrep

; 240 = LAPIC timer — lapic_timer_isr.asm (simbolnya bentrok).

%assign vec 241
%rep 12                     ; vektor 241..252
    ISR_DEFAULT vec
%assign vec vec+1
%endrep

; 253 = IPI reschedule — reschedule_isr.asm (simbolnya bentrok).

%assign vec 254
%rep 2                      ; vektor 254..255
    ISR_DEFAULT vec
%assign vec vec+1
%endrep

; ------------------------------------------------------------
; Tabel alamat stub default, dipakai idt.c untuk memasang SELURUH vektor
; 32..255 dalam satu loop.
;
; Kenapa tabel ini ada di sini, bukan 224 deklarasi `extern` di C:
;   * satu sumber kebenaran — daftar simbol dan tabelnya di-emit di sini;
;   * C tidak perlu tahu nama simbol `isrN_stub` per-vektor;
;   * vektor yang lupa didaftarkan jadi error NASM saat build, bukan IDT
;     entry nol yang baru ketahuan sebagai #GP → #DF saat runtime.
;
; KONTIGU: index 0 = vektor 32 ... index 223 = vektor 255.
;
; Tiga vektor punya file sendiri (128 syscall, 240 LAPIC timer, 253 IPI).
; Tabel di bawah merujuk stub itu lewat nama aslinya, sehingga tetap kontigu
; dan `table[v - 32]` benar. idt.c memasang handler khususnya ke slot yang
; sama, jadi entri ini hanya berfungsi sebagai pengisi slot.
extern isr128_stub
extern lapic_timer_isr_stub
extern reschedule_isr_stub

section .rodata
global isr_default_table
align 8
isr_default_table:
%assign vec 32
%rep 224
    %if vec == 128
        dq isr128_stub
    %elif vec == 240
        dq lapic_timer_isr_stub
    %elif vec == 253
        dq reschedule_isr_stub
    %else
        dq isr %+ vec %+ _stub
    %endif
%assign vec vec+1
%endrep
