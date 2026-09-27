/*
 * third_party/lexbor/kyuzen/src/lexbor_conv_shim.c
 *
 * Integer-only replacement for Lexbor's source/lexbor/core/conv.c.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The HTML/DOM parser closure needs exactly ONE symbol from Lexbor's
 * core/conv.c: lexbor_conv_int64_to_data() (called by core/bst.c in
 * lexbor_bst_serialize_entry(), which is reached via core/mraw.c's
 * allocation-cache bookkeeping). That function is pure integer math.
 *
 * The rest of core/conv.c, plus core/dtoa.c, core/strtod.c and
 * core/diyfp.c, implement IEEE-754 double <-> text conversion. They
 * RETURN double, so under the canonical Kyuzen userspace ABI
 * (--target=x86_64-pc-none-elf -mno-sse -msoft-float) they fail to
 * compile:
 *
 *     error: SSE register return with SSE disabled
 *
 * (verified empirically; see the Stage A report). Since KyuzenOS keeps
 * its userspace ABI SSE/FP-free and does not enable CR4.OSFXSR, the
 * whole FP conversion component is out of scope for this stage.
 *
 * We therefore ship only the integer-only function here and drop
 * conv.c/dtoa.c/strtod.c/diyfp.c from the build. The behaviour of
 * lexbor_conv_int64_to_data() is copied verbatim from upstream so it
 * stays bit-identical.
 *
 * The remaining declarations in core/conv.h (double-returning and
 * other converters) stay in the header for source compatibility, but
 * are never referenced by the HTML/DOM closure, so they are simply
 * left undefined and unreferenced. If a future stage needs them, the
 * right fix is an SSE/FP ABI decision at the kernel/toolchain level,
 * NOT weakening this shim.
 */

#include "lexbor/core/base.h"

size_t
lexbor_conv_int64_to_data(int64_t num, lxb_char_t *buf, size_t len)
{
    int64_t tmp;
    size_t have_minus, i, length;

    static const lxb_char_t *digits = (const lxb_char_t *) "0123456789";

    if (num != 0) {
        tmp = num;
        length = 0;
        have_minus = 0;

        if (num < 0) {
            length = 1;
            num = -num;
            have_minus = 1;
        }

        while (tmp != 0) {
            length += 1;
            tmp /= 10;
        }
    }
    else {
        if (len > 0) {
            buf[0] = '0';
            return 1;
        }

        return 0;
    }

    if (len < length) {
        i = (length + have_minus) - len;

        while (i != have_minus) {
            i -= 1;
            num /= 10;
        }

        length = len;
    }

    if (have_minus) {
        buf[0] = '-';
    }

    i = length;
    buf[length] = '\0';

    while (i != have_minus) {
        i -= 1;
        buf[i] = digits[ num % 10 ];
        num /= 10;
    }

    return length;
}
