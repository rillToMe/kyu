# Kyuzen C++ SDK (`libs/cpp/`)

Boundary publik untuk aplikasi C++ KyuzenOS — Phase 5 (runtime/foundation)
+ Phase 6 (subset libc++) + Phase 7 (antarmuka developer: header `kyuzen/`,
wrapper `kyuzen-c++`, contoh). Berdiri di atas Kyuzen C SDK (`libs/c/`):
header C, `libc.a`, dan CRT `_start` dipakai ulang; yang ditambahkan di sini
hanya milik C++.

## Tata letak

```text
libs/cpp/                   # COMMITTED: sumber boundary (bukan artifact)
├── include/
│   ├── __config_site       # site config libc++ (pengganti hasil CMake)
│   ├── __assertion_handler # assertion handler libc++ (adaptasi vendor)
│   └── kyuzen/
│       ├── config.hpp      # versi SDK + syarat bahasa (tanpa libc++)
│       ├── app.hpp         # tipe entry + helper kyuzen::run (CRT tetap otoritatif)
│       └── panic.hpp       # fail-hard kyuzen::panic (printf + abort)
├── bin/
│   └── kyuzen-c++          # sumber wrapper compiler (di-stage jadi executable)
├── linker/app.ld           # = app.ld C + section .init_array (ctor global)
└── README.md               # file ini

build/sdk/cpp/              # GENERATED (gitignored): hasil `make sdk-cpp`
├── include/                # closure libc++ (9 header publik subset +
│                           #   internal __* transitif + 2 site file Kyuzen +
│                           #   3 header publik <kyuzen/...>)
├── bin/kyuzen-c++          # wrapper compiler siap pakai (executable)
├── cxxrt.o                 # runtime C++ Phase 5 (new/delete, guard, ...)
├── lib/libcxxrt.a          # runtime libc++ Phase 6 (5 member, §18 audit)
└── linker/app.ld           # salinan linker script C++
```

`memory.hpp` SENGAJA tidak ada: `std::unique_ptr`/`std::make_unique` sudah
menyelesaikan masalah itu; SDK hanya menyediakan yang tidak ada di standar
(`config`/`app`/`panic` khas Kyuzen).

## Cara build aplikasi (satu-satunya yang perlu diketahui developer)

```sh
build/sdk/cpp/bin/kyuzen-c++ hello.cpp -o hello.elf
```

atau via make:

```sh
make cpp-app      # contoh examples/cpp/hello via wrapper publik
make cpp-app-run  # jalankan contoh hello di QEMU
make cpp-examples # semua contoh (hello/containers/strings)
```

Alur di balik wrapper (dimiliki SDK, bukan developer):

```text
hello.cpp
    ↓  flag kanonis + -isystem build/sdk/cpp/include + -isystem build/sdk/c/include
clang++ → object sementara (di TMPDIR, otomatis dibersihkan)
    ↓  ld.lld -m elf_x86_64 -nostdlib -T build/sdk/cpp/linker/app.ld
       app.o + lib/libcxxrt.a + cxxrt.o + crt.o + libc.a (urutan ini, selalu)
hello.elf  (statis, entry _start, 0 undefined symbol)
```

Flag kanonis = persis flag pembangun library (tertera di `kyuzen-c++ --help`):

```text
--target=x86_64-pc-none-elf -ffreestanding -nostdlib -nostdinc++
-fno-exceptions -fno-rtti -std=c++17 -O2
-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float
```

Catatan implementasi (bukan beban developer, didokumentasikan agar tak
terulang): link TIDAK lewat driver `clang++` — clang host ini mendelegasikan
link ke GCC/collect2 (gagal dengan `unrecognised emulation mode`), jadi
wrapper mengompilasi dengan `clang++` lalu me-link langsung dengan `ld.lld`,
persis seperti recipe Phase 5/6. Toolchain diambil dari `KYUZEN_CXX`/
`KYUZEN_LD` bila di-set Makefile, default `clang++`/`ld.lld` di PATH.

## Aturan (kontrak, bukan saran)

- `main` BERLINKAGE C: `extern "C" int main(int argc, char **argv)`
  (crt memanggil simbol `main`, bukan `_Z4mainiPPc`). `kyuzen::run` hanya
  helper dispatch — BUKAN entry kedua, startup tetap `_start` Phase 5
  (`RDI=argc/RSI=argv/RSP%16==0` tak berubah).
- Tanpa `throw`/`try`/`catch`, tanpa `typeid`/`dynamic_cast`,
  tanpa header di luar subset (`<iostream>` dkk. DITOLAK guard build;
  `<functional>` ter-stage tapi BUKAN API didukung).
- Subset Phase 6: `<array>`, `<algorithm>` (sort/find/find_if/copy/fill/
  min/max/swap — sort HANYA char/integer), `<memory>` (unique_ptr/
  make_unique), `<string>`, `<string_view>`, `<type_traits>`,
  `<utility>`, `<vector>`. Tanpa `stof`/`stod`, tanpa sort FP.
- `operator new` yang gagal → `abort()` (kebijakan `-fno-exceptions`;
  varian `nothrow` mengembalikan NULL). Arena malloc tetap 1 MiB.
- `kyuzen::panic(msg)` = printf + `abort()`; terminal, tak bisa ditangkap.
- Guard static-local = single-threaded (satu proses = satu thread).
- Pure virtual call → `abort()` (bug program).

## Batasan (lengkap di `docs/design/audit-llvm-libc-22-freestanding.md` §17–§19)

Phase 5: tanpa exceptions/libunwind, tanpa RTTI, tanpa libc++abi penuh,
tanpa STL, tanpa TLS, tanpa dynamic linking, tanpa libm C++.
Phase 6: subset di atas SAJA; tanpa iostream/filesystem/regex/locale/
thread/mutex/chrono/random/shared_ptr/atomic-sinkronisasi; tanpa
exceptions, RTTI, TLS; sort FP + stof/stod = di luar subset.
Phase 7: TIDAK memperluas libc++ — hanya membuat subset yang ada bisa
dipakai lewat boundary publik yang stabil.

## Perintah

```sh
make sdk-cpp            # stage SDK C++ ke build/sdk/cpp (+kyuzen/ +wrapper)
make sdk-cpp-smoke      # bangun app uji Phase 5 tools/libc-phase5 (murni via SDK)
make sdk-cpp-smoke-qemu # jalankan app uji Phase 5 di QEMU (harap PASS + dtor)
make libc-phase5        # alias: bangun app Phase 5
make libc-phase6        # bangun app uji Phase 6 tools/libc-phase6 (libc++ subset)
make libc-phase6-qemu   # jalankan app uji Phase 6 di QEMU (harap [phase6] PASS)
make cpp-app            # contoh hello via wrapper publik (harap link OK)
make cpp-app-run        # jalankan contoh hello di QEMU (harap sapaan SDK)
make cpp-examples       # semua contoh via wrapper publik
make cpp-sdk-isolation  # guard: 0 rujukan app ke internal LLVM/port/build
make libc-phase7        # smoke SDK publik Phase 7 via wrapper (harap link OK)
make libc-phase7-qemu   # jalankan smoke Phase 7 di QEMU (harap [phase7] PASS)
```
