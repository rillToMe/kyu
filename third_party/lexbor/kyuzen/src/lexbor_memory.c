/*
 * third_party/lexbor/kyuzen/src/lexbor_memory.c
 *
 * Kyuzen port of Lexbor's allocator hooks.
 *
 * Upstream Lexbor ships the allocator implementation under
 * source/lexbor/ports/<platform>/lexbor/core/memory.c. There is no
 * "none" port, so the archive would otherwise have undefined
 * lexbor_malloc/realloc/calloc/free. This file provides the port for
 * KyuzenOS userspace.
 *
 * Chain (per task spec):
 *
 *     Lexbor  ->  lexbor_malloc/realloc/calloc/free
 *             ->  libc malloc/realloc/calloc/free   (Kyuzen C SDK)
 *             ->  sys_alloc / sys_free               (kernel syscalls)
 *
 * Lexbor may also override the four function pointers at runtime via
 * lexbor_memory_setup(); the mechanism is preserved verbatim from the
 * upstream POSIX port so behaviour is identical to a hosted build.
 *
 * No POSIX, no hosted libc: only the freestanding Kyuzen SDK <stdlib.h>
 * (malloc/calloc/realloc/free) is used. The default initialisers below
 * are resolved at load time from the Kyuzen libc.
 */

#include "lexbor/core/base.h"

static lexbor_memory_malloc_f  lexbor_memory_malloc  = malloc;
static lexbor_memory_realloc_f lexbor_memory_realloc = realloc;
static lexbor_memory_calloc_f  lexbor_memory_calloc  = calloc;
static lexbor_memory_free_f    lexbor_memory_free    = free;

void *
lexbor_malloc(size_t size)
{
    return lexbor_memory_malloc(size);
}

void *
lexbor_realloc(void *dst, size_t size)
{
    return lexbor_memory_realloc(dst, size);
}

void *
lexbor_calloc(size_t num, size_t size)
{
    return lexbor_memory_calloc(num, size);
}

void *
lexbor_free(void *dst)
{
    lexbor_memory_free(dst);
    return NULL;
}

lxb_status_t
lexbor_memory_setup(lexbor_memory_malloc_f new_malloc,
                    lexbor_memory_realloc_f new_realloc,
                    lexbor_memory_calloc_f new_calloc,
                    lexbor_memory_free_f new_free)
{
    if (new_malloc == NULL || new_realloc == NULL
        || new_calloc == NULL || new_free == NULL)
    {
        return LXB_STATUS_ERROR_OBJECT_IS_NULL;
    }

    lexbor_memory_malloc  = new_malloc;
    lexbor_memory_realloc = new_realloc;
    lexbor_memory_calloc  = new_calloc;
    lexbor_memory_free    = new_free;

    return LXB_STATUS_OK;
}
