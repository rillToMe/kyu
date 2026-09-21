/* ============================================================
 * KyuzenOS — Phase 3 SDK smoke app (tools/libc-phase3/sdk_smoke.c)
 *
 * Aplikasi C REALISTIS yang dibangun MURNI lewat Kyuzen C SDK
 * (build/sdk/c): satu-satunya include libc adalah header SDK
 * (stdio/stdlib/string/stdint/errno/ctype/inttypes). File ini TIDAK
 * boleh menyebut path implementasi LLVM secara langsung — diverifikasi
 * oleh Makefile (`make sdk-c-smoke` gagal bila bocor ke internal LLVM).
 *
 * Diuji di QEMU via `start sdk_smoke hello` (argc==2, argv[1]=="hello"):
 *   [phase3] SDK startup ok   — _start → main(argc, argv) hidup
 *   [phase3] libc headers ok  — string/ctype/inttypes/errno
 *   [phase3] malloc ok        — malloc + snprintf + free
 *   [phase3] string ok        — strcpy/strcmp/strlen/memcpy
 *   [phase3] printf ok        — printf %d/%s/%x + return persis
 *   [phase3] argc/argv ok     — argv[1] == "hello", snprintf argc
 *   [phase3] PASS
 *
 * FAIL memakai syscall tulis mentah #49 agar terlihat walau stdio rusak
 * (ABI syscall Kyuzen adalah fondasi SDK, bukan internal LLVM).
 * ============================================================ */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long kz_write_raw(int fd, const void *buf, unsigned long n) {
  long ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"((long)49), "b"((long)fd), "c"(buf), "d"(n)
                   : "memory");
  return ret;
}

static void say_raw(const char *s) {
  size_t n = 0;
  while (s[n] != '\0')
    n++;
  (void)kz_write_raw(1, s, n);
}

#define FAIL(tag)                                                              \
  do {                                                                         \
    say_raw("[phase3] FAIL ");                                                 \
    say_raw(tag);                                                              \
    say_raw("\n");                                                             \
    return 42;                                                                 \
  } while (0)

#define CHECK(cond, tag)                                                       \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL(tag);                                                               \
  } while (0)

int main(int argc, char **argv) {
  printf("[phase3] SDK startup ok\n");

  /* Header + ctype + inttypes + errno (jalur libc sungguhan). */
  {
    CHECK(isdigit('7') != 0, "isdigit");
    CHECK(isalpha('q') != 0, "isalpha");
    CHECK(strlen("kyuzen") == 6, "strlen");
    errno = 0;
    (void)strtoimax("99999999999999999999999999", NULL, 10);
    CHECK(errno == ERANGE, "errno-ERANGE");
    printf("[phase3] libc headers ok\n");
  }

  /* malloc + snprintf + free. */
  {
    char *buffer = (char *)malloc(256);
    CHECK(buffer != NULL, "malloc-NULL");
    int n = snprintf(buffer, 256, "Kyuzen C SDK: argc=%d", argc);
    CHECK(n == 20, "malloc-snprintf-return");
    CHECK(strcmp(buffer, "Kyuzen C SDK: argc=2") == 0, "malloc-snprintf");
    printf("%s\n", buffer);
    free(buffer);
    printf("[phase3] malloc ok\n");
  }

  /* String functions. */
  {
    char dst[32];
    strcpy(dst, "sdk");
    strcat(dst, "-string");
    CHECK(strcmp(dst, "sdk-string") == 0, "strcpy-strcat");
    CHECK(memcmp(dst, "sdk-", 4) == 0, "memcmp");
    memcpy(dst, "OK!", 4); /* termasuk NUL: dst == "OK!" */
    CHECK(dst[0] == 'O' && dst[2] == '!' && dst[3] == '\0', "memcpy");
    printf("[phase3] string ok\n");
  }

  /* printf formatting + exact return. */
  {
    const char *expect = "[phase3] val=255 hex=ff str=sdk\n";
    int n = printf("[phase3] val=%d hex=%x str=%s\n", 255, 255, "sdk");
    CHECK(n == (int)strlen(expect), "printf-return");
    int m = fprintf(stderr, "[phase3] stderr ok\n");
    CHECK(m == (int)strlen("[phase3] stderr ok\n"), "fprintf-stderr");
    printf("[phase3] printf ok\n");
  }

  /* argc/argv dari kernel (start sdk_smoke hello). */
  {
    CHECK(argc == 2, "argc");
    CHECK(argv != NULL && argv[1] != NULL, "argv-null");
    CHECK(strcmp(argv[1], "hello") == 0, "argv1");
    printf("[phase3] argv0=%s\n", argv[0]);
    printf("[phase3] argc/argv ok\n");
  }

  printf("[phase3] PASS\n");
  return 0; /* → __llvm_libc_exit(0) → int 0x80 #34 */
}
