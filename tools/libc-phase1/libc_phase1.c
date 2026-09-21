/* ============================================================
 * KyuzenOS — Phase 1 smoke test (LLVM libc 22.1.8: exit + errno + malloc)
 *
 * Aplikasi user ELF statis yang menguji:
 *   1. malloc / calloc / realloc / free (LLVM libc di atas arena uheap Kyuzen)
 *   2. pola tulis-baca + isi lama realloc + zero-init calloc
 *   3. errno: tulis/baca lewat hook __llvm_libc_errno + jalur libc (strtoimax ERANGE)
 *   4. kasus gagal: permintaan melebihi arena → NULL
 *   5. exit: `return 0` → _start → __llvm_libc_exit(0) → int 0x80 #34
 *      dibuktikan dengan fork → anak `_Exit(7)` → waitpid membaca status 7
 *
 * Observability memakai syscall Kyuzen (fd 1 → TTY + mirror COM1). Phase 1
 * TIDAK menyentuh stdio/time/C++, jadi tidak ada printf di sini.
 *
 * Build: make libc-phase1 ; jalankan di QEMU: make libc-phase1-qemu
 * ============================================================ */

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* ------------------------------------------------------------
 * Syscall Kyuzen (int 0x80): RAX=nomor, RBX/RCX/RDX=arg1..3
 * ------------------------------------------------------------ */
static int kz_write(int fd, const void *buf, uint32_t n) {
  int64_t ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"((uint64_t)49), "b"((uint64_t)fd), "c"((uint64_t)buf), "d"((uint64_t)n));
  return (int)ret;
}

static int kz_fork(void) {
  int64_t ret;
  __asm__ volatile("int $0x80" : "=a"(ret) : "a"((uint64_t)78));
  return (int)ret;
}

static int kz_waitpid(int pid, int *status) {
  int64_t ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"((uint64_t)69), "b"((uint64_t)(int64_t)pid), "c"((uint64_t)status),
                     "d"((uint64_t)0));
  return (int)ret;
}

static void say(const char *s) {
  size_t n = 0;
  while (s[n] != '\0') n++;
  (void)kz_write(1, s, (uint32_t)n);
}

static void say_num(int32_t v) {
  char tmp[12];
  int i = 0;
  uint32_t u;
  if (v < 0) { say("-"); u = (uint32_t)(-(int64_t)v); } else u = (uint32_t)v;
  do { tmp[i++] = (char)('0' + (u % 10u)); u /= 10u; } while (u != 0);
  while (i > 0) (void)kz_write(1, &tmp[--i], 1);
}

/* Kegagalan apa pun: cetak penanda (terlihat di COM1/serial) lalu return 42
 * supaya status keluar yang bukan nol juga terlihat oleh parent. */
#define FAIL(tag) do { say("[phase1] FAIL "); say(tag); say("\n"); return 42; } while (0)

#define PATTERN_0(i) ((unsigned char)((i) * 7u + 1u))
#define PATTERN_1(i) ((unsigned char)((i) & 0xffu))

int main(void) {
  say("[phase1] start\n");

  /* 1. malloc 4 KiB + pola tulis/baca */
  unsigned char *p = (unsigned char *)malloc(4096);
  if (p == NULL)
    FAIL("malloc(4096)=NULL");
  for (uint32_t i = 0; i < 4096; i++) p[i] = PATTERN_0(i);
  for (uint32_t i = 0; i < 4096; i++)
    if (p[i] != PATTERN_0(i))
      FAIL("malloc-pattern");
  say("[phase1] ok malloc(4096)+pola\n");

  /* 2. realloc 4 KiB → 8 KiB: data lama harus utuh, area baru bisa dipakai */
  unsigned char *q = (unsigned char *)realloc(p, 8192);
  if (q == NULL)
    FAIL("realloc(8192)=NULL");
  for (uint32_t i = 0; i < 4096; i++)
    if (q[i] != PATTERN_0(i))
      FAIL("realloc-kehilangan-data");
  for (uint32_t i = 4096; i < 8192; i++) q[i] = PATTERN_1(i);
  for (uint32_t i = 0; i < 8192; i++)
    if (q[i] != (i < 4096 ? PATTERN_0(i) : PATTERN_1(i)))
      FAIL("realloc-pola");
  say("[phase1] ok realloc(8192)+data-utuh\n");

  /* 3. calloc harus men-zero-kan */
  unsigned char *c = (unsigned char *)calloc(64, 32);
  if (c == NULL)
    FAIL("calloc=NULL");
  for (uint32_t i = 0; i < 64u * 32u; i++)
    if (c[i] != 0)
      FAIL("calloc-tidak-zero");
  say("[phase1] ok calloc(64,32)=zero\n");

  /* 4. errno: hook tulis/baca, lalu jalur libc yang benar-benar menyetel errno */
  errno = 4242;
  if (errno != 4242)
    FAIL("errno-tulis-baca");
  errno = 0;
  (void)strtoimax("99999999999999999999999999", NULL, 10);
  if (errno != ERANGE)
    FAIL("errno-ERANGE-dari-libc");
  say("[phase1] ok errno (rw + ERANGE dari libc)\n");

  /* 5. kasus gagal: melebihi arena (1 MiB) → NULL. Catatan: malloc baremetal
   *    LLVM 22 tidak menyetel errno saat gagal (lihat dokumen audit). */
  void *huge = malloc(8u * 1024u * 1024u);
  if (huge != NULL)
    FAIL("malloc-melebihi-arena-tidak-NULL");
  say("[phase1] ok permintaan melebihi arena = NULL\n");

  /* 6. free + heap bisa dipakai ulang */
  free(q);
  free(c);
  void *r = malloc(512);
  if (r == NULL)
    FAIL("malloc-setelah-free");
  free(r);
  say("[phase1] ok free + reuse\n");

  /* 7. Bukti exit status berasal dari syscall #34: anak keluar lewat libc
   *    _Exit(7) (→ __llvm_libc_exit → int 0x80 #34) dan parent membacanya
   *    dengan waitpid. */
  int pid = kz_fork();
  if (pid < 0)
    FAIL("fork");
  if (pid == 0)
    _Exit(7); /* tidak kembali */
  int st = -1;
  int got = kz_waitpid(pid, &st);
  if (got != pid || st != 7) {
    say("[phase1] FAIL exit-status: got=");
    say_num(got);
    say(" status=");
    say_num(st);
    say("\n");
    return 42;
  }
  say("[phase1] ok exit(7) anak terbaca waitpid (syscall #34)\n");

  say("[phase1] PASS\n");
  return 0; /* → __llvm_libc_exit(0) → int 0x80 #34 */
}
