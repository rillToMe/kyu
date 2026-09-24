/***************************************************************************
 *
 * ftkz_stdlib.h — Kyuzen replacement for FreeType's ftstdlib.h.
 *
 * Dipakai via -DFT_CONFIG_STANDARD_LIBRARY_H="ftkz_stdlib.h"
 * (mekanisme resmi ftheader.h — tanpa patch upstream).
 *
 * Cakupan: HANYA yang dipakai set modul minimal Kyuzen
 * (truetype + sfnt + smooth + psnames + base inti + ftinit),
 * diinventarisasi di docs/design/font-rendering.md §2.
 *
 * - <stddef.h>/<limits.h> = header freestanding clang (selalu ada,
 *   bahkan untuk x86_64-pc-none-elf + -nostdlib).
 * - string/sort/strtol/getenv = kzf_* di kzf_port.c (freestanding,
 *   loop manual ala kernel/string.c — tanpa host libc).
 * - ft_jmp_buf/setjmp/longjmp = __builtin clang. Probe empiris
 *   (host clang O0+O2, x86_64): builtin menulis TEPAT 3 word di
 *   indeks 0..2; buffer 8 word = margin 5. longjmp upstream SELALU
 *   dipanggil dengan nilai literal 1 (ftobjs.c:175, pngshim.c:206 —
 *   grep-terverifikasi), jadi makro mengabaikan argumen nilai dan
 *   selalu melompat dengan 1. Semua call-site FT memeriksa `== 0`
 *   vs nonzero — perilaku identik.
 * - File I/O (FILE/fopen/fread/snprintf) SENGAJA tidak disediakan:
 *   Kyuzen memakai memory stream (FT_New_Memory_Face). ftsystem.c
 *   dan ftdebug.c hulu TIDAK dikompilasi.
 *
 */
#ifndef FTKZ_STDLIB_H_
#define FTKZ_STDLIB_H_

#include <stddef.h> /* size_t, ptrdiff_t, NULL */

#define ft_ptrdiff_t  ptrdiff_t

/* ---- integer limits (dari <limits.h> freestanding clang) ---- */
#include <limits.h>

#define FT_CHAR_BIT    CHAR_BIT
#define FT_USHORT_MAX  USHRT_MAX
#define FT_INT_MAX     INT_MAX
#define FT_INT_MIN     INT_MIN
#define FT_UINT_MAX    UINT_MAX
#define FT_LONG_MIN    LONG_MIN
#define FT_LONG_MAX    LONG_MAX
#define FT_ULONG_MAX   ULONG_MAX
#ifdef LLONG_MAX
#define FT_LLONG_MAX   LLONG_MAX
#endif
#ifdef LLONG_MIN
#define FT_LLONG_MIN   LLONG_MIN
#endif
#ifdef ULLONG_MAX
#define FT_ULLONG_MAX  ULLONG_MAX
#endif

/* ---- character and string processing (kzf_port.c) ---- */
void *kzf_memchr(const void *s, int c, size_t n);
int kzf_memcmp(const void *s1, const void *s2, size_t n);
void *kzf_memcpy(void *dest, const void *src, size_t n);
void *kzf_memmove(void *dest, const void *src, size_t n);
void *kzf_memset(void *s, int c, size_t n);
char *kzf_strcat(char *dest, const char *src);
int kzf_strcmp(const char *s1, const char *s2);
char *kzf_strcpy(char *dest, const char *src);
size_t kzf_strlen(const char *s);
int kzf_strncmp(const char *s1, const char *s2, size_t n);
char *kzf_strncpy(char *dest, const char *src, size_t n);
char *kzf_strrchr(const char *s, int c);
char *kzf_strstr(const char *haystack, const char *needle);

#define ft_memchr   kzf_memchr
#define ft_memcmp   kzf_memcmp
#define ft_memcpy   kzf_memcpy
#define ft_memmove  kzf_memmove
#define ft_memset   kzf_memset
#define ft_strcat   kzf_strcat
#define ft_strcmp   kzf_strcmp
#define ft_strcpy   kzf_strcpy
#define ft_strlen   kzf_strlen
#define ft_strncmp  kzf_strncmp
#define ft_strncpy  kzf_strncpy
#define ft_strrchr  kzf_strrchr
#define ft_strstr   kzf_strstr

/* ---- sorting (kzf_port.c: insertion sort, tabel FT kecil) ---- */
typedef int (*kzf_cmp_fn)(const void *, const void *);
void kzf_qsort(void *base, size_t nmemb, size_t size, kzf_cmp_fn compar);

#define ft_qsort  kzf_qsort

/* ---- memory allocation: TIDAK disediakan di sini ----
 * FT mengalokasi SELALU lewat FT_Memory (kz_ft_alloc di kzraster_ft.c
 * -> kz_heap_t). Simbol ft_smalloc dkk tidak direferensikan set minimal
 * (ftsystem.c tidak dikompilasi) — sengaja tanpa define. */

/* ---- miscellaneous ---- */
long kzf_strtol(const char *nptr, char **endptr, int base);
char *kzf_getenv(const char *name); /* selalu NULL: tanpa environment */

#define ft_strtol  kzf_strtol
#define ft_getenv  kzf_getenv

/* ---- execution control (clang builtins, tanpa libc) ---- */
typedef void *kzf_jmp_buf[8]; /* probe: builtin sentuh word 0..2 */

#define ft_jmp_buf  kzf_jmp_buf
#define ft_setjmp(b)  __builtin_setjmp((void **)(b))
#define ft_longjmp(b, v)  __builtin_longjmp((b), 1)

#endif /* FTKZ_STDLIB_H_ */

/* END */
