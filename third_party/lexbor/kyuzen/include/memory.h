/*
 * third_party/lexbor/kyuzen/include/memory.h
 *
 * Kyuzen port shim for Lexbor 3.0.0.
 *
 * Lexbor's lexbor/core/base.h unconditionally does:
 *
 *     #include <memory.h>
 *
 * <memory.h> is a POSIX/glibc convenience header (it just includes
 * <string.h>) and is NOT part of the freestanding Kyuzen C SDK
 * (build/sdk/c/include). Without this shim the preprocessor silently
 * falls back to the HOST libc header (/usr/include/memory.h in the
 * MSYS2 toolchain), which violates the "no hosted libc" rule even
 * though it happens to compile.
 *
 * This shim reproduces the exact semantics of the standard header
 * while resolving to the Kyuzen SDK <string.h>.
 */

#ifndef LEXBOR_KYUZEN_MEMORY_H
#define LEXBOR_KYUZEN_MEMORY_H

#include <string.h>

#endif /* LEXBOR_KYUZEN_MEMORY_H */
