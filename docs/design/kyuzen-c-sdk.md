# Kyuzen C SDK — public API boundary (Phase 3)

> Status: boundary publik di atas LLVM libc 22.1.8 baremetal yang terverifikasi
> Phase 0–2. Fase ini **infrastruktur SDK saja** — tidak ada ekspansi libc,
> tidak ada perubahan kernel/syscall ABI/ELF loader.
> Audit implementasi: `docs/design/audit-llvm-libc-22-freestanding.md` (§12–§14).

## 1. Struktur SDK

```text
sdk/c/                      # COMMITTED: sumber boundary
├── linker/app.ld           # linker script kanonis (ENTRY _start, 2 PT_LOAD @0x4000000)
└── README.md               # ringkasan boundary + perintah

build/sdk/c/                # GENERATED (gitignored): `make sdk-c`
├── include/                # salinan header hasil hdrgen (stdio/stdlib/string/...)
├── lib/libc.a              # salinan archive Phase 0–2 (197 member)
├── crt/crt.o               # salinan object port (_start + exit/errno/heap/stdio)
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

Hasil hdrgen Phase 0–2 (jangan klaim ISO C penuh):

```text
stdio.h stdlib.h string.h strings.h ctype.h errno.h
stdint.h inttypes.h stdbit.h locale.h
(+ float.h limits.h llvm-libc-macros/ llvm-libc-types/ sebagai dependensi generator)
```

Perilaku terverifikasi QEMU: `malloc/calloc/realloc/free`, `errno` (+`ERANGE`
via `strtoimax`), `exit/_Exit`, `printf/fprintf/snprintf/sprintf (+v-...)`,
`puts/fputs/putchar/fputc/getchar/fgetc/fgets/fread/fwrite`, `stderr` (fd 2),
`argc/argv` via `sys_spawn_argv`.

## 4. Limitasi (sinkron dengan audit §12–§14)

- Arena malloc tetap **1 MiB** (`KYUZEN_LIBC_ARENA_BYTES`); di atasnya → NULL.
- **stdin terpetakan tapi belum teruji runtime** (hook read → #48 ada;
  harness QEMU tak memberi input deterministik tanpa berebut fd 0).
- Tanpa `%f` (float printf dimatikan config baremetal).
- Tanpa keluarga `scanf`; tanpa `asprintf`; tanpa `putc/getc`.
- `feof`/`ferror` selalu 0 (stub upstream); tanpa file stream/seek/close/flush.
- `libc_errno` pasca-stdio-gagal = 1 (kernel mengembalikan -1 polos).
- Tanpa TLS (errno satu global per proses); tanpa POSIX/pthread; link statis;
  tanpa dynamic loader; tanpa C++ runtime.

## 5. Target make

```sh
make sdk-c             # stage SDK (+guard anti-stale: printf/malloc di libc.a)
make sdk-c-smoke       # app tools/libc-phase3 (murni SDK; guard anti-third_party)
make sdk-c-smoke-qemu  # QEMU otomatis, harap [phase3] PASS
```

Determinisme: `sdk-c` bergantung pada `libc.a` + object port, sehingga
perubahan `entrypoints.txt` menarik rebuild. **Pengecualian terdokumentasi**:
incremental CMake tidak mendeteksi perubahan `entrypoints.txt` (audit
§14.7.6) — bila guard `sdk-c` melaporkan `libc.a` basi, hapus
`build/libc/cmake` lalu ulangi dari `make libc-phase0`. Guard menolak state
basi dengan keras, tidak pernah diam-diam memakai archive lama.

## 6. Yang TIDAK dilakukan fase ini

Upgrade LLVM, ubah syscall ABI/loader, POSIX, dynamic linking, pthread, TLS,
C++/libc++, ekspansi stdio, libc pengganti, duplikasi header manual, salinan
kedua source LLVM. Sistem app lama (`user_apps/`, Rust via `app.ld`) tidak
disentuh dan tetap build.
