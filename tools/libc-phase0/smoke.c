/* ============================================================
 * KyuzenOS — LLVM libc 22.1.8 Phase 0 link smoke test
 *
 * Tujuan : membuktikan header generator + static archive + linkage bekerja
 *          untuk target x86_64-pc-none-elf (freestanding, tanpa OS).
 * Cakupan: fungsi pure saja — memcpy, memset, memcmp, strlen, strcmp,
 *          isalpha, isdigit.
 *
 * Batas Phase 0 (sengaja TIDAK dipakai di sini):
 *   printf / stdio  (butuh vendor hook __llvm_libc_stdio_*)
 *   malloc/free     (Phase 1)
 *   exit/_Exit      (butuh vendor hook __llvm_libc_exit)
 *   time            (butuh vendor hook __llvm_libc_timespec_get_*)
 *   _start / crt    (Phase 1)
 *
 * Kompilasi: clang --target=x86_64-pc-none-elf -ffreestanding -nostdlib
 * Link     : ld.lld -m elf_x86_64 -nostdlib --entry=phase0_entry ... -lc
 * Referensi: docs/design/audit-llvm-libc-22-freestanding.md
 * ============================================================ */

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Indirection volatile: mencegah clang melipat konstanta atau meng-inline loop
 * memcpy/memset, sehingga link test benar-benar memaksa simbol diambil dari
 * libc.a (bukan dibangkitkan sendiri oleh codegen). */
static volatile const char *v_src = "kyuzen llvm-libc 22.1.8 phase0";
static volatile size_t v_len = 0;
static volatile int v_alpha = 'K';
static volatile int v_digit = '7';

/* Buffer statis (bukan stack): entry dipanggil langsung oleh linker tanpa crt,
 * jadi tidak ada jaminan setup stack/ABI runtime. */
static char buf[64];
static uint32_t sink;

int phase0_entry(void) {
  const char *src = (const char *)v_src;

  v_len = strlen(src);
  if (v_len + 1u > sizeof(buf))
    return -1;

  memset(buf, 0, sizeof(buf));
  memcpy(buf, src, v_len + 1u);

  sink  = (uint32_t)strlen(buf);
  sink += (uint32_t)strcmp(buf, src);
  sink += (uint32_t)memcmp(buf, src, v_len);
  sink += isalpha(v_alpha) ? 1u : 0u;
  sink += isdigit(v_digit) ? 1u : 0u;

  return (int)(sink & 0xffu);
}
