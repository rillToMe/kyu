/***************************************************************************
 *
 * string.h — SHIM khusus build FreeType Kyuzen (B Whole-system header).
 *
 * Dua file upstream meng-include <string.h> LANGSUNG (bukan lewat
 * ftstdlib.h): src/base/md5.c (via ftobjs.c) dan src/smooth/ftgrays.c.
 * Phase 16 melarang patch upstream, jadi direktori ini (-I PALING
 * DEPAN, hanya untuk TU FreeType) menyediakan shim yang mengalihkan
 * memcpy/memset/memcmp/memmove/strlen ke implementasi kzf_* di
 * kzf_port.c (freestanding, tanpa host libc).
 *
 * JANGAN include header ini dari kode Kyuzen (kernel/app pakai
 * include/string.h repo). JANGAN menambah fungsi di sini selain yang
 * dipakai dua file di atas (memcpy/memset/memcmp + size_t).
 *
 */
#ifndef FTKZ_STRING_H_
#define FTKZ_STRING_H_

#include <stddef.h> /* size_t */

void *kzf_memcpy(void *dest, const void *src, size_t n);
void *kzf_memmove(void *dest, const void *src, size_t n);
void *kzf_memset(void *s, int c, size_t n);
int kzf_memcmp(const void *s1, const void *s2, size_t n);
size_t kzf_strlen(const char *s);

static inline void *memcpy(void *d, const void *s, size_t n) {
    return kzf_memcpy(d, s, n);
}
static inline void *memmove(void *d, const void *s, size_t n) {
    return kzf_memmove(d, s, n);
}
static inline void *memset(void *s, int c, size_t n) {
    return kzf_memset(s, c, n);
}
static inline int memcmp(const void *a, const void *b, size_t n) {
    return kzf_memcmp(a, b, n);
}
static inline size_t strlen(const char *s) { return kzf_strlen(s); }

#endif /* FTKZ_STRING_H_ */
