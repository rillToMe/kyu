bits 64
section .text
global gdt_flush
global tss_flush

gdt_flush:
    ; Di 64-bit System V ABI, argumen pertama bahasa C masuk ke register RDI
    lgdt [rdi]

    mov ax, 0x10    ; 0x10 adalah index ke-2 di GDT (Kernel Data)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Trik 64-bit untuk me-reload CS (Code Segment)
    push 0x08               ; Push CS selector (0x08)
    lea rax, [rel .flush]   ; Ambil alamat RIP tujuan
    push rax                ; Push alamat
    retfq                   ; Far Return (mengkonsumsi CS dan RIP dari stack)

.flush:
    ret

tss_flush:
    ; Index ke-5 di GDT x 8 byte = 40 (0x28)
    mov ax, 0x28
    ltr ax
    ret

; tss_flush_sel(uint16_t selector) — load TR dengan selector TSS eksplisit.
; Dipakai AP untuk memuat TSS milik CPU-nya sendiri (FIX_005 Tahap 1).
global tss_flush_sel
tss_flush_sel:
    mov ax, di
    ltr ax
    ret