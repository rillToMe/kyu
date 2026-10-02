# C++ SDK

The Kyuzen C++ SDK (`libs/cpp/`) is the public boundary for C++ applications.
It builds on the [C SDK](c-sdk.md): the C headers, `libc.a`, and the CRT
`_start` are reused, and only C++-specific pieces are added here.

## Layout

```text
libs/cpp/                   # COMMITTED: boundary source
├── include/
│   ├── __config_site       # libc++ site config
│   ├── __assertion_handler # libc++ assertion handler
│   └── kyuzen/
│       ├── config.hpp      # SDK version + language requirements
│       ├── app.hpp         # entry-point types + kyuzen::run helper
│       └── panic.hpp       # fail-hard kyuzen::panic
├── bin/kyuzen-c++          # compiler wrapper source
├── linker/app.ld           # C script + .init_array
└── README.md

build/target/sdk/cpp/       # GENERATED (gitignored): from `./build.sh sdk-cpp`
├── include/                # libc++ closure + Kyuzen headers
├── bin/kyuzen-c++          # ready-to-use wrapper
├── cxxrt.o                 # C++ runtime
├── lib/libcxxrt.a          # libc++ subset
└── linker/app.ld
```

## Building an Application

```sh
build/sdk/cpp/bin/kyuzen-c++ hello.cpp -o hello.elf
```

or via make:

```sh
./build.sh sdk-cpp      # stage the SDK first (see Commands below)
```

### What the wrapper does

```text
hello.cpp
    ↓  canonical flags + -isystem build/sdk/cpp/include + -isystem build/sdk/c/include
clang++ → temporary object
    ↓  ld.lld -m elf_x86_64 -nostdlib -T build/sdk/cpp/linker/app.ld
       app.o + libcxxrt.a + cxxrt.o + crt.o + libc.a   (this order, always)
hello.elf  (static, entry _start, 0 undefined symbols)
```

### Canonical flags

```text
--target=x86_64-pc-none-elf -ffreestanding -nostdlib -nostdinc++
-fno-exceptions -fno-rtti -std=c++17 -O2
-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float
```

> The link does **not** go through the `clang++` driver: on the host, clang
> delegates linking to GCC/collect2, which fails with `unrecognised emulation
> mode`. The wrapper compiles with `clang++` and links directly with `ld.lld`.

## C++ Runtime

`cxxrt.o` provides:

- `operator new`/`new[]` → `malloc` (a failed throwing `new` calls `abort()`;
  `nothrow` variants return NULL).
- `operator delete`/`delete[]` (including sized variants) → `free`.
- `__cxa_guard_acquire`/`release`/`abort` — single-threaded byte flag.
- `__dso_handle`.
- `__cxa_pure_virtual()` → `abort()`.
- **`__cxa_atexit` / `__cxa_finalize` / `atexit`** — owned by the C++ runtime,
  not `libc.a`. LLVM's version only finalizes `dso == NULL` registrants, but
  clang registers with `dso = &__dso_handle` (non-NULL), so global destructors
  would never run. The local implementation caps at 64 entries, runs LIFO, and
  is re-entrancy safe.
- `__throw_bad_alloc()` → `abort()`.

## libc++ Subset

Enabled public headers: `<array>`, `<algorithm>`, `<memory>`
(`unique_ptr`/`make_unique`), `<string>`, `<string_view>`, `<type_traits>`,
`<utility>`, `<vector>`, `<new>`.

- `<functional>` is staged as a build dependency but is **not** a supported
  API.
- `<iostream>` and similar headers are rejected by the build guard.

### Site config

`libs/cpp/include/__config_site`: ABI v1, threads disabled, locale/filesystem/
terminal/clock/unicode/widechar/tzdb disabled, hardening NONE, assertion
handler = trap.

### `libcxxrt.a`

Five members: `stdexcept.o`, `verbose_abort.o`, and three Kyuzen TUs that slice
(not reimplement) upstream behavior:

| Member | Reason |
| --- | --- |
| `shims.o` | `strtof`/`strtod`/`strtold` → `abort` (FP not available) |
| `string_inst.o` | Explicit `std::basic_string<char>` instantiation |
| `sort_inst.o` | Non-FP `__sort` (upstream `algorithm.cpp` instantiates FP sorts) |

The library is verified to contain no SSE/x87 instructions, no exceptions, no
unwinding, no pthreads, and no TLS.

## Application SDK Headers

| Header | Contents |
| --- | --- |
| `kyuzen/config.hpp` | `sdk_major = 7`, `sdk_minor = 0`, requires C++17 |
| `kyuzen/app.hpp` | `using app_main = int(int, char**)` + `kyuzen::run` helper |
| `kyuzen/panic.hpp` | `[[noreturn]] kyuzen::panic(msg)` = printf + `abort()` |

`main` must have **C linkage**: `extern "C" int main(int argc, char **argv)`.

## Rules (Contract)

- `main` is C-linkage; `kyuzen::run` is a dispatch helper, not a second entry
  point. Startup remains `_start`.
- No `throw`/`try`/`catch`, no `typeid`/`dynamic_cast`, no headers outside the
  subset.
- `operator new` failure → `abort()`; `nothrow` → NULL.
- Static-local guards are single-threaded (one process = one thread).
- A pure virtual call → `abort()`.

## Limitations

- No exceptions, no RTTI, no libc++abi, no full STL, no TLS, no dynamic
  linking, no C++ libm.
- Subset only: no iostream/filesystem/regex/locale/thread/mutex/chrono/random/
  shared_ptr/atomic-synchronization.
- FP sort, `stof`/`stod`/`strtof`/`strtod` are out of the subset (link error or
  hard `abort()`).
- `wchar_t` string operations are absent.

## Commands

```sh
./build.sh sdk-cpp      # stage the C++ SDK to build/target/sdk/cpp

> The old `sdk-cpp-smoke`, `sdk-cpp-smoke-qemu`, `cpp-app`, `cpp-app-run`,
> `cpp-examples` and `cpp-sdk-isolation` targets are not mapped to CMake yet,
> and the Makefile has been removed. Their example sources are still in
> `tools/libc-phase5/`, `tools/libc-phase6/`, `tools/libc-phase7/` and
> `examples/cpp/`.
```

## Related Documentation

- [C SDK](c-sdk.md)
- [libdesktop](../gui/libdesktop.md)
- [Shared Libraries](shared.md)
- [Building](../development/building.md)
