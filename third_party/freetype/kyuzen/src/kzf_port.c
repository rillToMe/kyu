/***************************************************************************
 *
 * kzf_port.c — Kyuzen freestanding primitives untuk FreeType.
 *
 * Menyediakan SEMUA simbol yang dijanjikan ftkz_stdlib.h + shim
 * string.h, tanpa satu pun header/fungsi host libc:
 *   - loop manual ala kernel/string.c (reuse pola, bukan duplikat
 *     link — kernel/string.c tetap milik kernel; file ini milik
 *     lib userspace + host test)
 *   - qsort = insertion sort (tabel FT kecil: hdmx, unimaps, woff
 *     indices — O(n^2) tak terasa, deterministik, tanpa rekursi)
 *   - strtol = basis 0/8/10/16 + tanda + endptr (pemakaian FT:
 *     property/int parsing, basis eksplisit 10)
 *   - getenv = selalu NULL (tanpa environment di freestanding;
 *     FT berperilaku default — benar secara semantik)
 *
 * Freestanding-clean: hanya <stddef.h> (clang). Tanpa alokasi global,
 * tanpa state, tanpa lock (pemanggil = single-flight per proses).
 *
 */
#include <stddef.h> /* size_t, NULL */

void *kzf_memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void *kzf_memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dest;
    if (d < s || d >= s + n) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}

void *kzf_memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

int kzf_memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

void *kzf_memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    }
    return NULL;
}

size_t kzf_strlen(const char *s) {
    size_t len = 0;
    while (s[len] != '\0') len++;
    return len;
}

int kzf_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int kzf_strncmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *kzf_strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0') {
    }
    return dest;
}

char *kzf_strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0') {
    }
    return dest;
}

char *kzf_strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

char *kzf_strrchr(const char *s, int c) {
    const char *last = NULL;
    char ch = (char)c;
    while (*s) {
        if (*s == ch) last = s;
        s++;
    }
    if (ch == '\0') return (char *)s;
    return (char *)last;
}

char *kzf_strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    size_t nlen = kzf_strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (*p == *needle && kzf_memcmp(p, needle, nlen) == 0)
            return (char *)p;
    }
    return NULL;
}

/* Insertion sort: cukup untuk tabel kecil FT, tanpa rekursi/qsort libc. */
void kzf_qsort(void *base, size_t nmemb, size_t size,
               int (*compar)(const void *, const void *)) {
    unsigned char *b = (unsigned char *)base;
    if (nmemb < 2 || size == 0 || !compar) return;
    /* ponytail: buffer swap 1 elemen di stack caller akan boros untuk
     * elemen besar — elemen FT (unimap/hdmx/woff index) <= 16 byte. */
    for (size_t i = 1; i < nmemb; i++) {
        size_t j = i;
        while (j > 0 && compar(b + j * size, b + (j - 1) * size) < 0) {
            /* swap elemen j dan j-1 byte-per-byte */
            for (size_t k = 0; k < size; k++) {
                unsigned char t = b[j * size + k];
                b[j * size + k] = b[(j - 1) * size + k];
                b[(j - 1) * size + k] = t;
            }
            j--;
        }
    }
}

static int kzf_digit_val(char c, int base, int *out) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else return 0;
    if (v >= base) return 0;
    *out = v;
    return 1;
}

long kzf_strtol(const char *nptr, char **endptr, int base) {
    const char *p = nptr;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\v' || *p == '\f')
        p++;
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    if (base == 0) {
        base = 10;
        if (*p == '0') {
            base = 8;
            p++;
            if (*p == 'x' || *p == 'X') {
                base = 16;
                p++;
            }
        }
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    long acc = 0;
    int any = 0, d;
    while (kzf_digit_val(*p, base, &d)) {
        acc = acc * base + d;
        any = 1;
        p++;
    }
    if (endptr) *endptr = (char *)(any ? p : nptr);
    return neg ? -acc : acc;
}

char *kzf_getenv(const char *name) {
    (void)name;
    return NULL; /* freestanding: tanpa environment */
}
