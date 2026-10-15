// R4 paging contract tests: pure leaf-PTE split + SHM range predicate.
// No page tables are walked; the kernel uses these exact helpers.
#include <stdint.h>
#include <stdio.h>

#include "paging.h"
#include "kz_shm.h"

static unsigned failures;
static void check(int ok, const char *label) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", label);
    }
}

int main(void) {
    uint64_t e, f;
    // NX-marked user data page: frame strips NX, flags keep it.
    e = 0x762E000ULL | PAGING_PRESENT | PAGING_WRITABLE | PAGING_USER |
        PAGING_NX;
    f = pte_frame(e);
    check(f == 0x762E000ULL, "frame strips NX");
    check((f & PAGING_NX) == 0, "frame has no flag bits");
    check(pte_flags(e) == (PAGING_PRESENT | PAGING_WRITABLE | PAGING_USER |
                           PAGING_NX),
          "flags preserve NX");
    // Executable page: no NX anywhere.
    e = 0x4000000ULL | PAGING_PRESENT | PAGING_USER;
    check(pte_frame(e) == 0x4000000ULL, "frame clean without NX");
    check(pte_flags(e) == (PAGING_PRESENT | PAGING_USER), "flags clean");
    // Frame + canonical HHDM: stripped frame keeps the sum canonical.
    check((pte_frame(e) + 0xFFFF800000000000ULL) < 0xFFFF800100000000ULL,
          "stripped frame maps canonical");
    {
        uint64_t wrapped =
            ((e | PAGING_NX) & 0xFFFFFFFFFFFFF000ULL) + 0xFFFF800000000000ULL;
        check(wrapped < 0x8000000000000000ULL, "raw mask wraps non-canonical");
    }
    // SHM range predicate (single source used by paging.c).
    check(!kz_shm_va_excluded(0x0ULL), "null not excluded");
    check(!kz_shm_va_excluded(0x4FFFFFFFULL), "below base mapped");
    check(kz_shm_va_excluded(KZ_SHM_BASE), "base excluded");
    check(kz_shm_va_excluded(KZ_SHM_END - 1), "end-1 excluded");
    check(!kz_shm_va_excluded(KZ_SHM_END), "end mapped");
    check(kz_shm_va_excluded(kz_shm_slot_va(15) + 4096u), "last slot excluded");
    printf("kz-pte failures=%u\n", failures);
    return failures ? 1 : 0;
}
