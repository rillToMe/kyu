/* ============================================================
 * Kyuzen OS — User App Linker Script
 * 
 * Tujuan: Hasilkan ELF64 dengan HANYA satu PT_LOAD segment
 * dimulai tepat dari 0x4000000, tanpa rogue PHDR segments.
 *
 * Masalah dengan -Ttext 0x4000000:
 *   ld.lld membuat PT_LOAD ekstra di 0x200000 (untuk PHDR table)
 *   → ELF loader memap 0x200000 → crash / corrupt low memory
 *
 * Solusi: Linker script eksplisit dengan PHDRS { } yang benar.
 * ============================================================ */

ENTRY(main)

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
