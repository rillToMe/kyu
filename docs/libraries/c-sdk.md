# C SDK

The Kyuzen C SDK (`libs/c/`) is the public boundary for C applications built on
**LLVM libc 22.1.8**, cross-compiled for the baremetal target
`x86_64-pc-none-elf`. Applications see only this SDK — not the vendored LLVM
sources or the LLVM build directory.

## Layout

```text
libs/c/                    # COMMITTED: boundary source (not an artifact)
├── linker/app.ld          # canonical linker script (ENTRY _start, 2 PT_LOAD)
├── libc-port/src/         # the port layer (see below)
└── README.md

build/target/sdk/c/        # GENERATED (gitignored): from `./build.sh sdk-c`
├── include/               # public LLVM libc headers
├── lib/libc.a             # libc archive
├── crt/crt.o              # startup + port layer
└── linker/app.ld          # copy of the canonical script
```

SDK artifacts are generated, never committed: `include/`, `lib/libc.a`, and
`crt/crt.o` are produced by `./build.sh sdk-c` from the pinned LLVM sources
(`llvmorg-22.1.8`). The only hand-written port source is
`libs/c/libc-port/src/kyuzen_libc_port.cpp`, compiled into `crt/crt.o`.

## Build Flow

```text
clang source.c
    ↓  -isystem build/sdk/c/include      (the ONLY libc include path)
Kyuzen SDK headers
    ↓  build/sdk/c/crt/crt.o             (automatic, never added manually)
Kyuzen CRT (_start → main → exit)
    ↓  build/sdk/c/lib/libc.a
libc.a
    ↓  -T build/sdk/c/linker/app.ld
app.elf  (static, entry _start, 0 undefined symbols)
```

### Canonical flags

```text
--target=x86_64-pc-none-elf -ffreestanding -nostdlib
-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -O2
```

The kernel never enables SSE (`CR4.OSFXSR` is off), so any `%xmm`/`st()` use
would raise `#UD`. These flags — identical to those used to build `libc.a` —
guarantee no such instructions are emitted.

## Startup

`_start` (from `crt.o`):

1. Receives `RDI = argc`, `RSI = argv`, aligns the stack (`and $-16, %rsp`).
2. Initializes the heap (`kyuzen_libc_heap_init`).
3. Walks `.init_array` (via weak `__init_array_start`/`__init_array_end`) so
   C++ global constructors run.
4. Calls `main(argc, argv)`.
5. Returns through `exit()`, which invokes `__llvm_libc_exit` (syscall 34).

## Port Layer

The port implements the vendor hooks LLVM libc expects:

| Hook | Implementation |
| --- | --- |
| `__llvm_libc_exit(int)` | Syscall 34 |
| `__llvm_libc_errno()` | A per-process global `int` (no TLS) |
| `__llvm_libc_stdio_read/write` | Syscalls 48/49 on fd 0/1/2 |
| `__llvm_libc_stdin/stdout/stderr_cookie` | Opaque structs carrying fd 0/1/2 |
| `__llvm_libc_timespec_get_active` | Syscall 14 (uptime ms, monotonic) |
| `__llvm_libc_timespec_get_utc` | Syscall 20 (RTC), converted to epoch |
| `malloc` / `free` / `calloc` / `realloc` / `aligned_alloc` | The Kyuzen heap (below) |

## Heap Allocator

The heap wraps LLVM's `FreeListHeap` in a **list of arenas**, because the
underlying allocator has a fixed ceiling and no grow hook.

| Constant | Value |
| --- | --- |
| `KYUZEN_LIBC_ARENA_BYTES` | 1 MiB (first arena + geometric unit) |
| `KYUZEN_LIBC_ARENA_MAX_BYTES` | 4 MiB (geometric ceiling) |
| `KYUZEN_LIBC_MAX_ARENAS` | 32 |
| `KYUZEN_LIBC_MAX_ALLOC_BYTES` | 64 MiB |
| `KYUZEN_LIBC_REGION_ALIGN` | 4096 |

- Each arena is one `sys_alloc` region plus one `FreeListHeap`.
- The first arena is taken lazily on first allocation; growth is geometric
  (1 → 2 → 4 MiB).
- A request above the geometric ceiling gets a dedicated arena sized to the
  request; dedicated arenas are freed when empty, geometric arenas are kept.
- `free`/`realloc` locate the owning arena by address range; foreign pointers
  are ignored.
- No locks (one process = one thread).

## Supported Headers

`stdio.h`, `stdlib.h`, `string.h`, `strings.h`, `ctype.h`, `errno.h`, `time.h`,
`stdint.h`, `inttypes.h`, `stdbit.h`, `locale.h`.

## C Runtime Additions

- **Time**: `clock`, `timespec_get(TIME_UTC)`, `gmtime[_r]`, `localtime[_r]`,
  `mktime`, `asctime[_r]`, `ctime[_r]`, `strftime[_l]`.
- **Stdlib**: `atoi/atol/atoll`, `strtol/strtoul/strtoll/strtoull`,
  `abs/labs/llabs`, `div/ldiv/lldiv`, `aligned_alloc`, `qsort/bsearch`,
  `rand/srand` (an xorshift PRNG — **not** a CSPRNG), `strdup/strndup`.

## Limitations

- **Heap grows on demand** (no 1 MiB cap), bounded by 32 arenas and 64 MiB per
  request.
- **No `%f`** (float printf is disabled); no `scanf` family, no `asprintf`, no
  `putc`/`getc`.
- `feof`/`ferror` always return 0 (upstream stubs); no file stream, seek,
  close, or flush.
- **No TLS**: `errno` is a single global per process.
- No POSIX, no pthreads, no dynamic linking.
- `stdin` is mapped but not runtime-tested.
- **Not implemented**: `time()`, `difftime` (returns `double`), `nanosleep`,
  `clock_gettime`/`clock_getres`/`clock_settime`, `gettimeofday`,
  `getenv`/`setenv`/`putenv`, `atof`/`strtod` family, all of libm.
- `remove()` is a stub returning `-1` (present only so libc++ `<cstdio>`
  compiles).

> **RTC quirk:** the CMOS clock is read as UTC and then offset by +7 hours
> (WIB). The hour wraps mod 24 **without day carry**, and the year is
> `BCD + 2000`. This is documented, not corrected.

## Commands

```sh
./build.sh sdk-c        # stage the SDK to build/target/sdk/c (with anti-stale asserts)

> The old `sdk-c-smoke` and `sdk-c-smoke-qemu` targets are not mapped to CMake
> yet, and the Makefile has been removed. Their example sources are still in
> `tools/libc-phase3/`.
```

> **Build gotcha:** incremental CMake does not detect `entrypoints.txt`
> changes. If you change entrypoints, remove `build/libc/cmake` and re-run
> `libc-phase0`.

## Related Documentation

- [C++ SDK](cpp-sdk.md)
- [Shared Libraries](shared.md)
- [Building](../development/building.md)
- [Applications](../userspace/applications.md)
