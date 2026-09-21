/* ============================================================
 * KyuzenOS — Phase 2 smoke test (LLVM libc 22.1.8: stdio baremetal)
 *
 * Aplikasi user ELF statis yang menguji console I/O LLVM libc di atas
 * hook port layer (__llvm_libc_stdio_read/write → syscall #48/#49):
 *   1. printf (format %d/%s/%x/%c/%u) + nilai return = panjang persis
 *   2. puts / putchar (return != EOF)
 *   3. snprintf + sprintf: verifikasi string hasil byte-per-byte
 *   4. fprintf ke stdout dan stderr
 *   5. fwrite ke stdout (return = jumlah item)
 *   6. interaksi malloc + stdio (snprintf ke buffer malloc + printf %s)
 *   7. stdin (fread/fgets/getchar) SENGAJA tidak diuji runtime: harness QEMU
 *      tidak menyediakan input deterministik tanpa berebut fd 0 dengan shell
 *      (lihat audit §14). Hook read tetap dipetakan ke syscall #48.
 *
 * Pelaporan FAIL memakai syscall tulis mentah (#49 fd 1) agar tetap terlihat
 * walau stdio yang sedang diuji rusak. Float (%f) tidak diuji: config
 * baremetal 22.1.8 mematikan float printf (DISABLE_FLOAT).
 *
 * Build: make libc-phase2 ; jalankan di QEMU: make libc-phase2-qemu
 * ============================================================ */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tulis mentah ke fd 1 — hanya untuk jalur FAIL (independen dari stdio). */
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
    say_raw("[phase2] FAIL ");                                                 \
    say_raw(tag);                                                              \
    say_raw("\n");                                                             \
    return 42;                                                                 \
  } while (0)

#define CHECK(cond, tag)                                                       \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL(tag);                                                               \
  } while (0)

int main(void) {
  printf("[phase2] start\n");

  /* 1. printf: format integer/string; return = jumlah char persis. */
  {
    const char *expect = "[phase2] num=-12345 str=kyuzen hex=ff ch=Q u=4294967295\n";
    int n = printf("[phase2] num=%d str=%s hex=%x ch=%c u=%u\n", -12345,
                   "kyuzen", 255, 'Q', 4294967295u);
    CHECK(n == (int)strlen(expect), "printf-return");
    printf("[phase2] printf ok\n");
  }

  /* 2. puts + putchar. (LLVM baremetal puts/putchar return 0, bukan char;
   *    yang diassert hanya "bukan EOF" sesuai kontrak C.) */
  {
    int r = puts("[phase2] puts ok");
    CHECK(r != EOF, "puts-EOF");
    int c = putchar('Z');
    CHECK(c != EOF, "putchar-EOF");
    putchar('\n');
  }

  /* 3. snprintf + sprintf: verifikasi buffer byte-per-byte SEBELUM PASS. */
  {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "int=%d str=%s hex=%x", -7, "hi", 255);
    CHECK(n == 20, "snprintf-return");
    CHECK(strcmp(buf, "int=-7 str=hi hex=ff") == 0, "snprintf-buffer");
    printf("[phase2] snprintf ok\n");

    char buf2[64];
    int m = sprintf(buf2, "a=%d b=%s", 1, "z");
    CHECK(m == 7, "sprintf-return");
    CHECK(strcmp(buf2, "a=1 b=z") == 0, "sprintf-buffer");
    printf("[phase2] sprintf ok\n");
  }

  /* 4. fprintf ke stdout + stderr (keduanya ter-mirror ke COM1). */
  {
    const char *eout = "[phase2] fprintf stdout ok\n";
    int n = fprintf(stdout, "[phase2] fprintf %s ok\n", "stdout");
    CHECK(n == (int)strlen(eout), "fprintf-stdout");
    int m = fprintf(stderr, "[phase2] fprintf stderr ok\n");
    CHECK(m == (int)strlen("[phase2] fprintf stderr ok\n"), "fprintf-stderr");
  }

  /* 5. fwrite ke stdout: return = jumlah item penuh. */
  {
    const char msg[] = "[phase2] fwrite ok\n";
    size_t w = fwrite(msg, 1, sizeof(msg) - 1, stdout);
    CHECK(w == sizeof(msg) - 1, "fwrite-count");
  }

  /* 6. malloc + stdio: snprintf ke buffer heap, cetak via printf %s. */
  {
    char *p = (char *)malloc(64);
    CHECK(p != NULL, "malloc-NULL");
    int n = snprintf(p, 64, "m=%d", 1234);
    CHECK(n == 6, "malloc-snprintf-return");
    CHECK(strcmp(p, "m=1234") == 0, "malloc-snprintf-buffer");
    printf("[phase2] malloc+stdio %s\n", p);
    free(p);
  }

  printf("[phase2] PASS\n");
  return 0; /* → __llvm_libc_exit(0) → int 0x80 #34 */
}
