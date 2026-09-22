/* ============================================================
 * Kyuzen OS — C SDK application linker script (libs/c/linker/app.ld)
 *
 * Canonical script for C apps built against the Kyuzen C SDK
 * (LLVM libc 22.1.8 baremetal + port layer). Compatible with the Kyuzen
 * ELF loader: static ELF64, two PT_LOAD segments at 0x4000000, BSS
 * zeroed by the loader, entry `_start` from the SDK CRT (RDI=argc,
 * RSI=argv, RSP%16==0).
 *
 * Minimal on purpose: no PHDR/INTERP/NOTE/TLS segments — the loader only
 * understands PT_LOAD, and .note/.gnu/.eh_frame are discarded.
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
