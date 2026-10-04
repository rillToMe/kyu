bits 64
section .text
global idt_flush

idt_flush:
    ; Argumen dari C (pointer idtp) berada di register RDI
    lidt [rdi]        
    ret