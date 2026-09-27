/* ============================================================
 * Kyuzen OS - Linker script untuk probe Lexbor Stage A
 *
 * Sama dengan tools/libc-phase1/libc_app.ld: satu PT_LOAD di 0x4000000
 * (R-X) + satu PT_LOAD data (RW-); .eh_frame/.note/.gnu dibuang.
 * Entry point `_start` dari port layer libc Kyuzen
 * (libs/c/libc-port/src/kyuzen_libc_port.cpp), bukan `main`:
 * kernel melompat ke e_entry dengan RDI=argc, RSI=argv; `_start` memanggil
 * main lalu meneruskan return value ke __llvm_libc_exit (#34).
 *
 * app.ld milik aplikasi biasa TIDAK diubah.
 * ============================================================ */

ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R-X: code + rodata */
    data PT_LOAD FLAGS(6);   /* RW-: data + bss    */
}

SECTIONS {
    . = 0x4000000;

    /* ---- TEXT segment (executable, read-only data) ---- */
    .text   : { *(.text .text.*) }     :text
    .rodata : { *(.rodata .rodata.*) } :text

    /* ---- DATA segment (read-write) ---- */
    . = ALIGN(4096);
    .data : { *(.data .data.*) }       :data
    .bss  : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }                                  :data

    /* ---- Buang semua debug / metadata sections ---- */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.gnu*)
        *(.eh_frame*)
        *(.debug*)
    }
}
