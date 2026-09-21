# Kyuzen C SDK — public API boundary (Phase 3–4)

> Status: boundary publik di atas LLVM libc 22.1.8 baremetal yang terverifikasi
> Phase 0–4. Phase 3 = **infrastruktur SDK saja**; Phase 4 = **ekspansi runtime
> C** (time + utilitas) tanpa perubahan kernel/syscall ABI/ELF loader.
> Audit implementasi: `docs/design/audit-llvm-libc-22-freestanding.md` (§12–§16).

## 1. Struktur SDK

```text
sdk/c/                      # COMMITTED: sumber boundary
├── linker/app.ld           # linker script kanonis (ENTRY _start, 2 PT_LOAD @0x4000000)
└── README.md               # ringkasan boundary + perintah

build/sdk/c/                # GENERATED (gitignored): `make sdk-c`
├── include/                # salinan header hasil hdrgen (stdio/stdlib/string/time/...)
├── lib/libc.a              # salinan archive Phase 0–4 (232 member)
├── crt/crt.o               # salinan object port (_start + exit/errno/heap/stdio/time)
└── linker/app.ld           # salinan linker script kanonis
```

Keputusan: `include/` + `libc.a` + `crt.o` **tidak di-commit** (repo
meng-gitignore `build/`). Tidak ada header yang diduplikasi manual; tidak ada
source LLVM yang disalin — SDK men-stage dari sumber yang di-pin
(`llvmorg-22.1.8` di `third_party/stdlib/llvm-project/`). Satu-satunya sumber
port tetap `libs/libc-port/src/kyuzen_libc_port.cpp` (di luar tree LLVM).

## 2. Alur compile/link kanonis

```sh
clang --target=x86_64-pc-none-elf -ffreestanding -nostdlib \
      -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -O2 \
      -isystem build/sdk/c/include -c app.c -o app.o
ld.lld -m elf_x86_64 -nostdlib -T build/sdk/c/linker/app.ld \
      -o app.elf app.o build/sdk/c/crt/crt.o build/sdk/c/lib/libc.a
```

Developer tidak menambahkan object port manual — `crt.o` masuk dari alur
linker SDK. Startup: `_start` (RDI=argc, RSI=argv, `and $-16,%rsp`) →
`kyuzen_libc_heap_init()` → `main` → `__llvm_libc_exit` (#34). ABI
`RDI=argc/RSI=argv/RSP%16==0` dipertahankan; ELF loader tidak diubah.

## 3. Header standar yang didukung

Hasil hdrgen Phase 0–4 (jangan klaim ISO C penuh):

```text
stdio.h stdlib.h string.h strings.h ctype.h errno.h time.h
stdint.h inttypes.h stdbit.h locale.h
(+ float.h limits.h llvm-libc-macros/ (+baremetal/) llvm-libc-types/ sebagai dependensi generator)
```

Perilaku terverifikasi QEMU: `malloc/calloc/realloc/free`, `errno` (+`ERANGE`
via `strtoimax`), `exit/_Exit`, `printf/fprintf/snprintf/sprintf (+v-...)`,
`puts/fputs/putchar/fputc/getchar/fgetc/fgets/fread/fwrite`, `stderr` (fd 2),
`argc/argv` via `sys_spawn_argv` (Phase 0–3), ditambah Phase 4 (§4a).

## 4. Limitasi (sinkron dengan audit §12–§16)

- Arena malloc tetap **1 MiB** (`KYUZEN_LIBC_ARENA_BYTES`); di atasnya → NULL.
- **stdin terpetakan tapi belum teruji runtime** (hook read → #48 ada;
  harness QEMU tak memberi input deterministik tanpa berebut fd 0).
- Tanpa `%f` (float printf dimatikan config baremetal).
- Tanpa keluarga `scanf`; tanpa `asprintf`; tanpa `putc/getc`.
- `feof`/`ferror` selalu 0 (stub upstream); tanpa file stream/seek/close/flush.
- `libc_errno` pasca-stdio-gagal = 1 (kernel mengembalikan -1 polos).
- Tanpa TLS (errno satu global per proses); tanpa POSIX/pthread; link statis;
  tanpa dynamic loader; tanpa C++ runtime.

### 4a. Tambahan Phase 4 (terverifikasi `[phase4] PASS`)

**Waktu** (`time.h`; backend §5):
`clock`, `timespec_get` (hanya `TIME_UTC`), `gmtime`/`gmtime_r`,
`localtime`/`localtime_r` (= UTC, tanpa tz database — perilaku upstream),
`mktime`, `asctime`/`asctime_r`, `ctime`/`ctime_r`, `strftime`/`strftime_l`.

**Utilitas stdlib** (murni userspace): `atoi`/`atol`/`atoll`,
`strtol`/`strtoul`/`strtoll`/`strtoull` (+`ERANGE`/`endptr`),
`abs`/`labs`/`llabs`, `div`/`ldiv`/`lldiv`, `aligned_alloc`,
`qsort`/`bsearch`, `rand`/`srand` (PRNG xorshift saja, BUKAN CSPRNG).

**String alokasi**: `strdup`/`strndup` (via LLVM malloc → arena #9).

**Tetap ditunda (alasan di audit §16)**: `time()`, `difftime` (return double —
tidak bisa dikompilasi `-mno-sse` di sisi pemanggil), `nanosleep`,
`clock_gettime`/`clock_getres`/`clock_settime`, `gettimeofday` (tanpa backend
baremetal di 22.1.8), `getenv`/`setenv`/`putenv` (tanpa model environment),
`atof`/`strtod(f/l)` (float parsing + libcall FP), varian `_l`, `qsort_r`,
`a64l`/`l64a`, seluruh libm (kebijakan tanpa-SSE/x87).

## 5. Backend waktu (Phase 4)

```text
clock() / active time  →  syscall #14 uptime ms (timer_get_ms, uint64)
timespec_get(TIME_UTC) →  syscall #20 RTC [thn,bln,hari,jam,menit,detik]
                          → epoch via days-from-civil (integer murni)
```

Semantik (jangan disamakan): **uptime BUKAN wall clock** (nol saat boot,
monotonik, tanpa zona waktu); UTC berpresisi detik (`tv_nsec = 0`) dari RTC
yang dilaporkan sebagai WIB (+7 jam, bungkus jam tanpa carry hari — quirk
kernel yang didokumentasikan, bukan diperbaiki, fase ini).

## 6. Target make

```sh
make sdk-c             # stage SDK (+guard anti-stale: printf/malloc/qsort/timespec_get di libc.a, time.h ada)
make sdk-c-smoke       # app tools/libc-phase3 (murni SDK; guard anti-third_party)
make sdk-c-smoke-qemu  # QEMU otomatis, harap [phase3] PASS
make libc-phase4       # app tools/libc-phase4 via SDK (bukti runtime baru terekspos)
make libc-phase4-qemu  # QEMU otomatis, harap [phase4] PASS
```

Determinisme: `sdk-c` bergantung pada `libc.a` + object port, sehingga
perubahan `entrypoints.txt` menarik rebuild. **Pengecualian terdokumentasi**:
incremental CMake tidak mendeteksi perubahan `entrypoints.txt` (audit
§14.7.6) — bila guard `sdk-c` melaporkan `libc.a` basi, hapus
`build/libc/cmake` lalu ulangi dari `make libc-phase0`. Guard menolak state
basi dengan keras, tidak pernah diam-diam memakai archive lama.

## 7. Yang TIDAK dilakukan fase ini

Upgrade LLVM, ubah syscall ABI/loader, POSIX, dynamic linking, pthread, TLS,
C++/libc++, ekspansi stdio, libc pengganti, duplikasi header manual, salinan
kedua source LLVM. Sistem app lama (`user_apps/`, Rust via `app.ld`) tidak
disentuh dan tetap build.

## 8. Fondasi C++ — Phase 5–6 (bukan bagian C SDK)

C++ tinggal di boundary terpisah `sdk/cpp/` (dokumen: `sdk/cpp/README.md`,
audit §17–§18). Dari sudut pandang C, SDK C **hampir tak berubah**:
satu-satunya delta Phase 6 adalah stub `remove` di libc.a (selalu -1;
dibutuhkan using-declaration libc++ `<cstdio>` — audit §18.3). App C tidak
memakainya; perilaku C identik (bukti: regresi Phase 1–4 hijau).

Ringkasan Phase 5: `operator new/delete` (+sized/nothrow) → malloc/free;
guard static-local single-thread; `__dso_handle`; `__cxa_pure_virtual`;
`__cxa_atexit/finalize` milik runtime C++ (versi `libc.a` hanya memfinalisasi
`dso==NULL` sementara clang mendaftar `dso=&__dso_handle`); `<new>` minimal;
linker C++ = script C + `.init_array`; `main` wajib linkage C;
`-fno-exceptions -fno-rtti -nostdinc++`. Tanpa STL/libc++/unwind/RTTI/TLS.

Ringkasan Phase 6: subset libc++ — `<array>`, `<algorithm>`, `<memory>`
(unique_ptr/make_unique), `<string>`, `<string_view>`, `<type_traits>`,
`<utility>`, `<vector>` (+ `<functional>` hanya sebagai dependensi build,
bukan API didukung). Site config + assertion handler milik Kyuzen;
closure header otomatis via `clang -M`; arsip `libcxxrt.a` 5 member
(stdexcept, verbose_abort + 3 TU slice Kyuzen: shims FP-abort, instantiation
`basic_string<char>`, instantiation `__sort` non-FP). Alokasi =
`std::allocator → operator new → malloc → FreeListHeap → #9`; `__throw_*`
= abort/verbose-abort keras; sort FP + stof/stod di luar subset.

## 9. Application SDK — Phase 7 (boundary developer)

Phase 7 tidak menambah SATU PUN header/fungsi libc++: ia mengubah fondasi
Phase 5–6 menjadi SDK yang bisa dipakai developer tanpa tahu path LLVM,
urutan arsip, atau flag (`sdk/cpp/README.md`, audit §19). Publik:

- Header `<kyuzen/config.hpp>` (versi SDK 7.0, syarat C++17; tanpa libc++),
  `<kyuzen/app.hpp>` (tipe `app_main` + helper `kyuzen::run`; CRT `_start`
  Phase 5 tetap otoritatif — bukan entry kedua), `<kyuzen/panic.hpp>`
  (`panic(msg)` = printf + abort; terminal karena `-fno-exceptions`).
  `memory.hpp` SENGAJA tidak ada (`std::make_unique` sudah cukup).
- Wrapper `build/sdk/cpp/bin/kyuzen-c++` (sumber: `sdk/cpp/bin/kyuzen-c++`):
  satu perintah `kyuzen-c++ app.cpp -o app.elf` memberi flag kanonis,
  dua `-isystem` SDK, kompilasi ke object sementara, lalu link `ld.lld`
  langsung dengan urutan `app → libcxxrt.a → cxxrt.o → crt.o → libc.a`
  (`-T app.ld` C++). Link tidak lewat driver clang (clang host
  mendelegasikan link ke GCC — didokumentasikan di README).
- Contoh `examples/cpp/{hello,containers,strings}` — ELF asli via wrapper.
- Guard `tools/libc-phase7/check-sdk-isolation.sh`: 0 rujukan source app ke
  `third_party/stdlib/llvm-project`, `libs/libc-port`, `build/libc`,
  `build/libcxx`. Smoke `tools/libc-phase7` + `make libc-phase7-qemu`
  (`[phase7] PASS`), `make cpp-app(-run)`.
