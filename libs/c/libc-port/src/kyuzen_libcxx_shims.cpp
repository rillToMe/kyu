// ============================================================
// KyuzenOS — shims milik Kyuzen untuk subset libc++ Phase 6.
//
// Isi: stub abort() untuk fungsi C yang DIREFERENSIKAN TU libc++ yang
// dikompilasi TETAPI di luar subset C yang didukung — tepatnya
// strtof/strtod/strtold, dipakai tak-langsung oleh stof/stod/stold di
// libcxx/src/string.cpp (as_float_helper). Tanpa simbol-simbol ini,
// string.o gagal link; tanpanya, stof/stod tak bisa dipakai (lihat bawah).
//
// MENGAPA STUB ASM, BUKAN FUNGSI C:
//   Tipe kembalian float/double/long-double TIDAK BISA dikompilasi dengan
//   flag SDK (-mno-sse -msoft-float): clang menolak ("SSE register return
//   with SSE disabled" — pelajaran yang sama dengan difftime Phase 4).
//   Stub asm `jmp abort` tidak menyentuh register FP sama sekali.
//
// SEMANTIK (jujur, bukan palsu):
//   Memanggil stof/stod/stold (atau strtof/strtod/strtold langsung) =
//   abort() seketika. Parsing FP memang DITUNDA di C SDK (tanpa SSE/x87,
//   tanpa tabel strtofloat). Bila C library kelak menyediakan yang asli,
//   hapus file ini — simbol asli menang tanpa perubahan app.
// ============================================================

// NOLINT: definisi simbol global via asm tingkat atas — disengaja.
// Referensi `abort` di bawah diselesaikan linker dari LLVM libc (Phase 1);
// tidak ada deklarasi C yang dibutuhkan karena tidak ada kode C di sini.
__asm__(
    ".text\n"
    ".globl strtof\n"
    ".type strtof, @function\n"
    "strtof:\n"
    "  jmp abort\n"
    ".globl strtod\n"
    ".type strtod, @function\n"
    "strtod:\n"
    "  jmp abort\n"
    ".globl strtold\n"
    ".type strtold, @function\n"
    "strtold:\n"
    "  jmp abort\n");
