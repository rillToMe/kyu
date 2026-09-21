/* ============================================================
 * Kyuzen OS — C++ SDK application linker script (sdk/cpp/linker/app.ld)
 *
 * Identik dengan sdk/c/linker/app.ld KECUALI section .init_array:
 * constructor global C++ (yang tidak ter-fold konstanta) tinggal di sini
 * dan dijalankan _start SEBELUM main. Destruktor global didaftarkan
 * compiler via __cxa_atexit (sudah di libc.a) dan dijalankan exit().
 *
 * Kompatibel dengan Kyuzen ELF loader: statis ELF64, dua PT_LOAD di
 * 0x4000000, BSS di-zero loader, entry `_start` (RDI=argc, RSI=argv,
 * RSP%16==0). App C yang di-link script ini tetap jalan (init_array
 * kosong = tidak ada yang dijalankan) — tapi app C WAJIB memakai script
 * sdk/c (boundary tidak berubah dari sudut pandang C).
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

    /* ---- Init array C++: fungsi void(int,char**,char**) ---- */
    /* Simbol __init_array_start/end dibaca _start via weak reference; */
    /* app C (tanpa section ini) melihat keduanya NULL → dilewati.     */
    .init_array : {
        __init_array_start = .;
        *(.init_array .init_array.*)
        *(.ctors .ctors.*)
        __init_array_end = .;
    }                                  :text

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
