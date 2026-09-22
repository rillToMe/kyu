/* ============================================================
 * Kyuzen OS — Linker script app yang memakai LLVM libc (Phase 1)
 *
 * Sama dengan apps/app.ld (satu PT_LOAD di 0x4000000 + satu PT_LOAD
 * data; .eh_frame/.note/.gnu dibuang) dengan SATU perbedaan: entry point
 * adalah `_start` dari port layer libc (libs/c/libc-port/src/kyuzen_libc_port.cpp),
 * bukan `main`. Kernel melompat ke e_entry dengan RDI=argc, RSI=argv; `_start`
 * lalu memanggil main dan meneruskan return value ke __llvm_libc_exit (#34).
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
