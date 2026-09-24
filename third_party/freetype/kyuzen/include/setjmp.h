/***************************************************************************
 *
 * setjmp.h — SHIM khusus build FreeType Kyuzen.
 *
 * src/smooth/ftgrays.c meng-include <setjmp.h> langsung tetapi TIDAK
 * memanggil setjmp/longjmp (grep-terverifikasi). Shim ini hanya
 * memenuhi include tersebut. API setjmp yang dipakai FT (ftobjs.c,
 * ttcmap.c) datang dari ftkz_stdlib.h (clang builtins), bukan dari sini.
 *
 * JANGAN include header ini dari kode Kyuzen.
 *
 */
#ifndef FTKZ_SETJMP_H_
#define FTKZ_SETJMP_H_

/* Sengaja kosong selain guard: tidak ada pemakai setjmp() di TU yang
 * meng-include header ini. Bila suatu hari ada, arahkan ke
 * ftkz_stdlib.h (ft_jmp_buf/ft_setjmp/ft_longjmp), bukan ke sini. */

#endif /* FTKZ_SETJMP_H_ */
