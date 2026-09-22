# Kyuzen C SDK (`libs/c/`)

Boundary publik untuk aplikasi C KyuzenOS di atas LLVM libc 22.1.8
(baremetal `x86_64-pc-none-elf`). Aplikasi hanya mengenal SDK ini —
bukan `third_party/stdlib/llvm-project` dan bukan direktori build LLVM.

## Tata letak

```text
libs/c/                     # COMMITTED: sumber boundary (bukan artifact)
├── linker/app.ld           # linker script kanonis (ENTRY _start, 2 PT_LOAD)
└── README.md               # file ini

build/sdk/c/                # GENERATED (gitignored): hasil `make sdk-c`
├── include/                # header publik LLVM libc (disalin dari hasil hdrgen)
├── lib/libc.a              # archive terverifikasi Phase 0–4
├── crt/crt.o               # startup + port layer (_start, exit/errno/heap/stdio/time)
└── linker/app.ld           # salinan linker script kanonis
```

Keputusan staging (jangan di-commit ke `libs/c/`):

- `include/` + `lib/libc.a` + `crt/crt.o` adalah **artifact build** —
  repo meng-gitignore `build/`, jadi SDK selalu di-stage ulang dari sumber
  LLVM yang di-pin (`llvmorg-22.1.8`) lewat `make sdk-c`. Tidak ada header
  yang diduplikasi manual; tidak ada source LLVM yang disalin ke SDK.
- Satu-satunya sumber port adalah `libs/c/libc-port/src/kyuzen_libc_port.cpp`
  (di luar tree LLVM); SDK mengompilasinya menjadi `crt/crt.o`.

## Alur build aplikasi

```text
clang source.c
    ↓  -isystem build/sdk/c/include   (SATU-SATUNYA include libc)
Kyuzen SDK headers
    ↓  build/sdk/c/crt/crt.o          (otomatis, jangan ditambah manual)
Kyuzen CRT (_start → main → exit)
    ↓  build/sdk/c/lib/libc.a
libc.a
    ↓  -T build/sdk/c/linker/app.ld
app.elf  (statis, entry _start, 0 undefined symbol)
```

Flag kanonis (jangan tambah flag Linux; jangan nyalakan SSE):

```text
--target=x86_64-pc-none-elf -ffreestanding -nostdlib
-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -O2
```

Kernel tidak mengaktifkan SSE (`CR4.OSFXSR` mati) — kode app/libc yang
memakai `%xmm`/`st()` akan `#UD`. Flag di atas (sama persis dengan yang
dipakai membangun `libc.a`) menjamin tidak ada instruksi itu.

## Batasan (ringkas; lengkap di `docs/design/kyuzen-c-sdk.md`)

Arena malloc tetap 1 MiB; stdin terpetakan tapi belum teruji runtime;
tanpa `%f`, tanpa keluarga `scanf`, tanpa file stream/seek/close/flush;
`feof`/`ferror` stub; errno tanpa TLS; link statis; tanpa POSIX/pthread.
Phase 4 menambah: `time.h` (clock/timespec_get UTC via #14/#20 — uptime
BUKAN wall clock; tanpa `time()`/`nanosleep`/`clock_gettime`), `strtol`-
family/`atoi`/`abs`/`div`-family/`qsort`/`bsearch`/`rand`/`srand` (PRNG saja),
`strdup`/`strndup`/`aligned_alloc`; tanpa `getenv`/`setenv`, tanpa libm.
Phase 6 menambah satu stub C: `remove()` (selalu -1, tanpa FS — dibutuhkan
agar header libc++ `<cstdio>` dapat dikompilasi; app C tidak memakainya).

C++ BUKAN bagian boundary ini — tinggal di `libs/cpp/` (fondasi Phase 5–6 +
application SDK Phase 7: header `kyuzen/`, wrapper `kyuzen-c++`, contoh;
tanpa STL/libc++ baru, tanpa exception/RTTI).
Dari sudut pandang C tidak ada yang berubah (bukti: regresi Phase 1–4 hijau
setelah `_start` melayani `.init_array` C++ via weak symbol).

## Perintah

```sh
make sdk-c             # stage SDK ke build/sdk/c (+assert anti-stale)
make sdk-c-smoke       # bangun app uji tools/libc-phase3 (murni via SDK)
make sdk-c-smoke-qemu  # jalankan app uji di QEMU (harap [phase3] PASS)
```
