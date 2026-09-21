/* ============================================================
 * KyuzenOS — Phase 4 C runtime smoke app (tools/libc-phase4/sdk_smoke.c)
 *
 * Dibangun MURNI lewat Kyuzen C SDK (build/sdk/c): satu-satunya include
 * libc adalah header SDK (stdio/stdlib/string/time/...). File ini TIDAK
 * boleh menyebut path implementasi LLVM secara langsung — diverifikasi
 * Makefile (guard menolak bila ada rujukan ke dalam tree LLVM).
 *
 * Backend waktu (port layer, tanpa syscall baru):
 *   clock() / active  → syscall #14 uptime ms  (MONOTONIK, bukan wall clock)
 *   timespec_get UTC  → syscall #20 RTC [thn,bln,hari,jam(WIB),mnt,dtk]
 *                       → epoch via days-from-civil (integer murni)
 *
 * Diuji di QEMU via `start libc_phase4`:
 *   [phase4] time API ok
 *   [phase4] monotonic/uptime semantics ok
 *   [phase4] calendar ok
 *   [phase4] numeric conversion ok
 *   [phase4] qsort ok
 *   [phase4] bsearch ok
 *   [phase4] strdup ok
 *   [phase4] random API ok
 *   [phase4] PASS
 *
 * Setiap `ok` didahului assertion runtime eksak. FAIL memakai syscall tulis
 * mentah #49 agar terlihat walau stdio rusak.
 * ============================================================ */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    say_raw("[phase4] FAIL ");                                                 \
    say_raw(tag);                                                              \
    say_raw("\n");                                                             \
    return 42;                                                                 \
  } while (0)

#define CHECK(cond, tag)                                                       \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL(tag);                                                               \
  } while (0)

static int cmp_int(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

int main(void) {
  /* ---- time API: timespec_get(TIME_UTC) via RTC #20 ---- */
  {
    struct timespec t1, t2;
    CHECK(timespec_get(&t1, TIME_UTC) == TIME_UTC, "ts-utc-ret1");
    CHECK(timespec_get(&t2, TIME_UTC) == TIME_UTC, "ts-utc-ret2");
    /* RTC tahun = BCD + 2000 → epoch tidak mungkin di bawah 2000-01-01. */
    CHECK(t1.tv_sec >= 946684800L, "ts-utc-range");
    CHECK(t2.tv_sec >= t1.tv_sec, "ts-utc-mono");
    CHECK(t1.tv_nsec == 0 && t2.tv_nsec == 0, "ts-utc-nsec");
    /* Base selain TIME_UTC ditolak jujur (return 0), bukan diarang. */
    CHECK(timespec_get(&t1, 12345) == 0, "ts-bad-base");
    printf("[phase4] time API ok\n");
  }

  /* ---- monotonik: clock() via uptime #14 ---- */
  {
    clock_t c1 = clock();
    clock_t c2 = clock();
    CHECK(c1 >= 0, "clock-neg");
    CHECK(c2 >= c1, "clock-mono");
    printf("[phase4] monotonic/uptime semantics ok\n");
  }

  /* ---- kalender murni-userspace: gmtime/mktime/strftime/asctime/ctime ---- */
  {
    /* 2024-01-01 00:00:00 UTC = 1704067200 (Senin). */
    /* NOTE: gmtime_r LLVM 22.1.8 memakai (time_t *, ...) non-const. */
    time_t fixed = (time_t)1704067200L;
    struct tm tmv;
    CHECK(gmtime_r(&fixed, &tmv) != NULL, "gmt-null");
    CHECK(tmv.tm_year == 124 && tmv.tm_mon == 0 && tmv.tm_mday == 1,
          "gmt-ymd");
    CHECK(tmv.tm_hour == 0 && tmv.tm_min == 0 && tmv.tm_sec == 0,
          "gmt-hms");
    CHECK(tmv.tm_wday == 1, "gmt-wday");
    /* localtime == UTC di baremetal (tanpa tz database, upstream). */
    {
      struct tm ltv;
      CHECK(localtime_r(&fixed, &ltv) != NULL, "loc-null");
      CHECK(ltv.tm_year == tmv.tm_year && ltv.tm_mon == tmv.tm_mon &&
                ltv.tm_mday == tmv.tm_mday && ltv.tm_hour == tmv.tm_hour &&
                ltv.tm_min == tmv.tm_min && ltv.tm_sec == tmv.tm_sec,
            "loc-eq-utc");
    }
    /* mktime invers gmtime (tanpa offset zona waktu). */
    {
      struct tm back = tmv;
      CHECK(mktime(&back) == fixed, "mktime-roundtrip");
    }
    /* Epoch 0 → 1970-01-01 Kamis. */
    {
      time_t zero = (time_t)0;
      struct tm z;
      CHECK(gmtime_r(&zero, &z) != NULL, "gmt0-null");
      CHECK(z.tm_year == 70 && z.tm_mon == 0 && z.tm_mday == 1 &&
                z.tm_wday == 4,
            "gmt0-fields");
    }
    /* strftime deterministik. */
    {
      char buf[32];
      CHECK(strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv) == 19,
            "fmt-ret");
      CHECK(strcmp(buf, "2024-01-01 00:00:00") == 0, "fmt-str");
    }
    /* asctime/ctime format "%a %b %e %T %Y\n". */
    {
      const char *as = asctime(&tmv);
      CHECK(as != NULL, "asctime-null");
      CHECK(strcmp(as, "Mon Jan  1 00:00:00 2024\n") == 0, "asctime-str");
      {
        time_t zero = (time_t)0;
        const char *cs = ctime(&zero);
        CHECK(cs != NULL, "ctime-null");
        CHECK(strcmp(cs, "Thu Jan  1 00:00:00 1970\n") == 0, "ctime-str");
      }
    }
    printf("[phase4] calendar ok\n");
  }

  /* ---- konversi numerik: strtol-family + atoi + abs + div ---- */
  {
    char *end = NULL;
    errno = 0;
    CHECK(strtol("123", &end, 10) == 123L, "stol-pos");
    CHECK(end != NULL && *end == '\0', "stol-end");
    CHECK(strtol("-456", NULL, 10) == -456L, "stol-neg");
    CHECK(strtol("0", NULL, 10) == 0L, "stol-zero");
    CHECK(strtol("0xff", NULL, 16) == 255L, "stol-hex16");
    CHECK(strtol("ff", NULL, 16) == 255L, "stol-hex");
    CHECK(strtol("010", NULL, 0) == 8L, "stol-oct0");
    CHECK(strtol("0x10", NULL, 0) == 16L, "stol-hex0");
    CHECK(strtoul("4294967295", NULL, 10) == 4294967295UL, "stoul-max32");
    CHECK(strtoll("-9223372036854775807", NULL, 10) ==
              -9223372036854775807LL,
          "stoll-neg");
    CHECK(strtoull("18446744073709551615", NULL, 10) ==
              18446744073709551615ULL,
          "stoull-max64");
    /* Overflow: LONG_MAX + ERANGE + endptr maju. */
    {
      errno = 0;
      char *oend = NULL;
      long ov = strtol("99999999999999999999999999", &oend, 10);
      CHECK(ov == LONG_MAX, "stol-ovf-val");
      CHECK(errno == ERANGE, "stol-ovf-errno");
      CHECK(oend != NULL && *oend == '\0', "stol-ovf-end");
    }
    /* Underflow: LONG_MIN + ERANGE. */
    {
      errno = 0;
      long un = strtol("-99999999999999999999999999", NULL, 10);
      CHECK(un == LONG_MIN, "stol-unf-val");
      CHECK(errno == ERANGE, "stol-unf-errno");
    }
    CHECK(atoi("  -42xyz") == -42, "atoi");
    CHECK(atol("123456789012") == 123456789012L, "atol");
    CHECK(atoll("-987654321098") == -987654321098LL, "atoll");
    CHECK(abs(-5) == 5 && abs(0) == 0, "abs");
    CHECK(labs(-123456789012L) == 123456789012L, "labs");
    CHECK(llabs(-987654321098LL) == 987654321098LL, "llabs");
    {
      div_t d = div(7, 3);
      CHECK(d.quot == 2 && d.rem == 1, "div-pos");
      div_t dn = div(-7, 3);
      CHECK(dn.quot == -2 && dn.rem == -1, "div-neg");
      ldiv_t ld = ldiv(1000000L, 7L);
      CHECK(ld.quot == 142857L && ld.rem == 1L, "ldiv");
      lldiv_t lld = lldiv(10000000000LL, 3LL);
      CHECK(lld.quot == 3333333333LL && lld.rem == 1LL, "lldiv");
    }
    printf("[phase4] numeric conversion ok\n");
  }

  /* ---- qsort ---- */
  {
    int a[] = {5, 2, 8, 1, 9, 3};
    const int want[] = {1, 2, 3, 5, 8, 9};
    qsort(a, 6, sizeof(int), cmp_int);
    CHECK(memcmp(a, want, sizeof(a)) == 0, "qsort-order");
    /* Edge: satu elemen + sudah terurut. */
    {
      int one[] = {7};
      qsort(one, 1, sizeof(int), cmp_int);
      CHECK(one[0] == 7, "qsort-one");
      int sorted[] = {1, 2, 3};
      qsort(sorted, 3, sizeof(int), cmp_int);
      CHECK(sorted[0] == 1 && sorted[2] == 3, "qsort-sorted");
    }
    printf("[phase4] qsort ok\n");
  }

  /* ---- bsearch (di atas array terurut) ---- */
  {
    const int a[] = {1, 2, 3, 5, 8, 9};
    int key = 8;
    int *hit =
        (int *)bsearch(&key, a, 6, sizeof(int), cmp_int);
    CHECK(hit != NULL && *hit == 8, "bsearch-hit");
    {
      int miss = 7;
      CHECK(bsearch(&miss, a, 6, sizeof(int), cmp_int) == NULL,
            "bsearch-miss");
    }
    printf("[phase4] bsearch ok\n");
  }

  /* ---- strdup/strndup (+ aligned_alloc di atas heap yang sama) ---- */
  {
    const char *orig = "kyuzen-dup";
    char *dup = strdup(orig);
    CHECK(dup != NULL, "strdup-null");
    CHECK(dup != orig, "strdup-alias");
    CHECK(strcmp(dup, orig) == 0, "strdup-cmp");
    dup[0] = 'K';
    CHECK(orig[0] == 'k' && dup[0] == 'K', "strdup-copy");
    free(dup);
    {
      char *t = strndup("hello-world", 5);
      CHECK(t != NULL && strcmp(t, "hello") == 0, "strndup-trunc");
      free(t);
      char *f = strndup("hi", 10);
      CHECK(f != NULL && strcmp(f, "hi") == 0, "strndup-short");
      free(f);
    }
    /* aligned_alloc: blok selaras 64 dari freelist yang sama. */
    {
      void *p = aligned_alloc(64, 128);
      CHECK(p != NULL, "aligned-null");
      CHECK(((uintptr_t)p % 64U) == 0, "aligned-mod");
      free(p);
    }
    printf("[phase4] strdup ok\n");
  }

  /* ---- random: PRNG xorshift deterministik (BUKAN CSPRNG) ---- */
  {
    int a1, a2, b1, b2;
    srand(42);
    a1 = rand();
    a2 = rand();
    srand(42);
    b1 = rand();
    b2 = rand();
    CHECK(a1 == b1 && a2 == b2, "rand-reseed");
    CHECK(a1 != a2, "rand-progress");
    CHECK(a1 >= 0 && a1 <= RAND_MAX && a2 >= 0 && a2 <= RAND_MAX,
          "rand-range");
    printf("[phase4] random API ok\n");
  }

  printf("[phase4] PASS\n");
  return 0; /* → __llvm_libc_exit(0) → int 0x80 #34 */
}
