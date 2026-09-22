# Audit: LLVM libc Freestanding x86_64 Backend untuk KyuzenOS

> **Status**: audit only — tidak ada implementasi yang dilakukan.
> **Sumber**: LLVM **24.0.0** vendored di `third_party/stdlib/llvm-project/libc/`.
> **Kebijakan**: tidak ada perubahan kernel; semua pekerjaan port berada di sisi build-config LLVM dan shim userspace.

---

## 1. Bagaimana LLVM libc mendefinisikan "platform backend"

Dari `libc/docs/porting.md` + `libc/cmake/modules/LLVMLibCArchitectures.cmake`:

1. **Triple** menentukan OS: `x86_64-pc-none-elf` / `x86_64-unknown-none-elf` → `LIBC_TARGET_OS = "none"` → **baremetal** (LLVMLibCArchitectures.cmake:160). Triple ini **identik** dengan yang sudah dipakai Makefile kernel & `apps/Makefile` — tidak perlu triple baru.
2. **Config dir**: `libc/config/baremetal/` berisi `config.json` (errno external, thread single, printf tanpa float, math no-errno/no-except) + subdirektori per-arch `entrypoints.txt` + `headers.txt`. Yang ada: `aarch64`, `arm`, `riscv`. **Gap**: belum ada `config/baremetal/x86_64/`.
3. **OSUtil layer**: `libc/src/__support/OSUtil/baremetal/` **tidak punya lapisan syscall sama sekali** — semua akses OS lewat *vendor hooks* `extern "C"` yang disediakan port layer (lihat §4).
4. **Startup**: `libc/startup/baremetal/init.cpp`+`fini.cpp` (arch-neutral) menjalankan `__preinit_array`/`__init_array`/`__fini_array`. `crt1` per-arch hanya ada untuk aarch64/arm — untuk x86_64 CMake hanya warning *"Cannot build 'crt1.o' for x86_64 yet"*. Artinya `_start` + pemanggilan `main` menjadi tugas shim KyuzenOS.
5. **Threads**: `LIBC_THREAD_MODE_SINGLE` (baremetal config.json) → tidak ada kebutuhan clone/futex/TLS sama sekali di fase ini.

## 2. Syscall / ABI yang sudah tersedia di kernel

Konvensi trap (dari `kernel/syscall.c` + `libs/core/userlib.c`):

```
int $0x80   ; RAX = nomor, RBX = arg1, RCX = arg2, RDX = arg3, RSI = arg4, RDI = arg5
            ; return di RAX; semua GPR lain dipertahankan kernel (PUSHA64/POPA64)
```

⚠️ Ini **bukan** konvensi Linux (RDI/RSI/RDX/R10/R8/R9) dan bukan SysV — shim syscall harus ditulis khusus, `syscall.h` Linux tidak bisa direuse.

### 2.1 Tabel syscall (80 nomor) — relevansi ke LLVM libc

| Nomor | Nama | Fungsi bagi port libc |
|---|---|---|
| 1 | print | (legacy; stdio lewat 49) |
| 3 | read_keyboard | alternatif stdin (TTY) |
| 4 | yield | idle hook (opsional) |
| 5–8, 11–13, 18, 24, 64 | fs_format/list/read/delete/exists/size/read_buffer/create/list/mkdir | sumber data `FILE*` custom (out-of-scope fase awal) |
| **9/10/19** | **sys_alloc / sys_free / sys_realloc** (uheap per-proses, region page-granular, base 0x10000000) | **backing malloc** |
| **14** | **uptime (ms, monotonic)** | **CLOCK_MONOTONIC** |
| **20** | **get_time (RTC [y,m,d,h,min,s])** | **TIME_UTC (presisi detik)** |
| 29–32, 40, 58–67, 80 | GUI/KWM/GPU/crash-notice | tidak dibutuhkan libc |
| 34 | exit(code) | **exit hook** |
| 38/39 | shutdown/reboot | (tidak dipakai libc) |
| **46** | **sleep(ms)** | sleep shim (opsional) |
| **47–51** | **open/read/write/lseek/close (fd)** | **stdio cookies (stdin 0 / stdout 1 / stderr 2 sudah dipasang kernel per task)** |
| 52–56 | socket TCP | tidak dibutuhkan fase awal |
| 57, 68, 77, 78, 79 | spawn/spawn_argv/spawn_redir/fork/execve | tidak dibutuhkan baremetal subset |
| 69–73 | waitpid/getpid/getppid/proc_list/kill | tidak dibutuhkan baremetal subset |
| 74/75/76 | dup/dup2/pipe | tidak dibutuhkan baremetal subset |

### 2.2 ABI proses & memori yang relevan

- Entry: `RDI = argc`, `RSI = argv` (P0 Phase 2) — **sudah SysV-compliant** untuk pemanggilan `main`; shim `_start` cukup meneruskan register.
- Stack: IRETQ entry → `RSP % 16 == 0` di `_start` — sesuai SysV process-entry requirement.
- ELF loader (`kernel/proc/elf.c`) **hanya mem-parse `PT_LOAD`** (p_type == 1); zero-fill `p_memsz > p_filesz` (BSS) sudah benar. `PT_TLS`/`PT_DYNAMIC` diabaikan → binari harus statis, tanpa TLS — **cocok** untuk freestanding subset.
- `uheap`: region page-granular `[0x10000000, 0x40000000)`, plafon alokasi tunggal 64 MB, model brk/region (bukan mmap generik).
- **Tidak ada**: auxv, environ, signal, vdso, TLS syscall, mmap/mprotect generik, futex, clock_gettime ns.

## 3. Vendor hooks yang WAJIB disediakan port layer (sisi userspace, tanpa ubah kernel)

| Hook | Lokasi referensi di LLVM libc | Backing KyuzenOS |
|---|---|---|
| `__llvm_libc_exit(int)` | `src/__support/OSUtil/baremetal/exit.cpp` | `int 0x80` #34 `sys_exit_code` |
| `__llvm_libc_stdio_write/read(cookie, buf, size)` | `src/stdio/baremetal/file_internal.h` | `int 0x80` #49/#48 pada fd 0/1/2 |
| objek `__llvm_libc_stdin/stdout/stderr_cookie` (type `struct __llvm_libc_stdio_cookie`) | `src/stdio/baremetal/stdout.cpp` dst. | struct + 3 objek di shim |
| `__llvm_libc_timespec_get_utc(ts)` | `src/time/baremetal/timespec_get.cpp` | #20 RTC (detik) → epoch perlu konversi days-from-civil di shim |
| `__llvm_libc_timespec_get_active(ts)` | `src/time/baremetal/clock.cpp` | #14 uptime ms → tv_sec/tv_nsec |
| definisi `errno` (`LIBC_ERRNO_MODE_EXTERNAL`) | `config/baremetal/config.json` | global `int` di shim (single-thread, aman) |
| simbol linker `_end`, `__llvm_libc_heap_limit` | `src/__support/freelist_heap.h` | tambahan `app.ld` (bagian linker script app, bukan kernel) |
| simbol `__init_array_start/end`, `__fini_array_*` | `startup/baremetal/init.cpp` | tambahan `app.ld` — saat ini `app.ld` **tidak** men-emit `.init_array` (malah discard `.gnu*`) |
| `_start` + call `main` | (crt1 x86_64 tidak ada upstream) | asm shim di port layer |
| heap instance | `freelist_heap` (`src/stdlib/baremetal/malloc.cpp` memanggil `freelist_heap->allocate`) | default `_end..__llvm_libc_heap_limit`; alternatif: instansiasi `FreeListHeap(span)` di atas region `sys_alloc(9)` |

## 4. Dependency matrix

Legend status: **✅** tersedia / **🔶** bisa via shim userspace tanpa ubah kernel / **⚠️** parsial (semantik/presisi menyimpang) / **❌** belum ada (gap kernel — dihindari dari subset).

| Grup entrypoint libc (baremetal) | Primitive platform yang dibutuhkan | Sumber di KyuzenOS | Status |
|---|---|---|---|
| string.h, strings.h (memcpy/memset/strlen/strcmp/dll) | tidak ada (pure) | — | ✅ |
| ctype.h, wctype.h, stdbit.h, uchar | tidak ada (pure) | — | ✅ |
| inttypes.h (strtoimax dst), stdlib int (atoi/strtol/qsort/bsearch/rand) | tidak ada (pure) | — | ✅ |
| locale.h ("C" locale: localeconv/setlocale) | tidak ada (pure) | — | ✅ |
| regex (experimental) | tidak ada (pure) | — | ✅ (opsional) |
| stdlib malloc/calloc/free/realloc/aligned_alloc/malloc_usable_size | memori user region | #9/10/19 (uheap) **atau** freelist heap statis via simbol linker | 🔶 |
| stdlib exit/_Exit/abort/atexit | exit hook; trap | #34; `__builtin_trap()`→#UD→`exception_handler` (kernel, tanpa diubah) | 🔶 |
| compiler entrypoints (`__stack_chk_fail`) | stack protector | baremetal config matikan strong SSP; app dibangun tanpa `-fstack-protector` | 🔶 |
| stdio.h (printf/puts/fwrite/fread/fscanf/stdin/stdout/stderr) | write/read callback + 3 cookie | #49/#48 pada fd 0/1/2 (stdio TTY sudah dipasang kernel per task) | 🔶 |
| time.h `clock`, `timespec_get` | clock source monotonic + wall | #14 (ms) + #20 (RTC detik) | ⚠️ granularitas ms/detik, `tv_nsec` tidak presisi |
| time.h gmtime/mktime/strftime/localtime | konversi kalender (pure) + tz (UTC saja) | — | ✅ (UTC) |
| errno | objek external | definisi shim | 🔶 |
| fenv.h (float/double: fegetround dst.) | `stmxcsr/ldmxcsr` → butuh **CR4.OSFXSR** | kernel hanya set SMEP/SMAP/CR0.WP, **tidak pernah set OSFXSR** → #UD | ❌ (hindari; x87-control-word path bisa dipelajari kemudian) |
| math.h / libm (acos…trunc, scalar fp) | FP arithmetic | `-msoft-float -mno-sse` → x87; baremetal config `LIBC_MATH_NO_ERRNO/NO_EXCEPT/SKIP_ACCURATE_PASS` | ⚠️ kompilabel, tapi presisi x87 (extended precision) berbeda dari IEEE-double yang diasumsikan algoritma — perlu validasi; alternatif jangka panjang: enable SSE (kernel change, out-of-scope) |
| threads.h / pthread | **tidak dibutuhkan** (`LIBC_THREAD_MODE_SINGLE`) | — | ✅ (non-issue) |
| TLS (`_Thread_local`, `errno` TLS) | **tidak dibutuhkan** (errno external; PT_TLS diabaikan loader) | — | ✅ (non-issue untuk subset) |
| unistd/fcntl (open/read/write POSIX) | **tidak masuk** headers.txt baremetal | fd syscalls tersedia bila nanti config diperluas | ✅ (out-of-scope) |
| `nanosleep`/`sleep` | timer | #46 sleep(ms) | ⚠️ ms saja; tidak masuk subset baremetal default |
| OSUtil syscall layer | lapisan syscall | **baremetal tidak punya**; ABI int 0x80 kustom → shim per-fungsi | 🔶 |

### 4.1 Dependency build/config (LLVM side, bukan kernel)

| Item | Status | Keterangan |
|---|---|---|
| Triple `x86_64-pc-none-elf` → baremetal | ✅ | otomatis via LLVMLibCArchitectures.cmake |
| `config/baremetal/x86_64/{entrypoints,headers}.txt` | ❌ (belum ada) | salin/sepakat dari `riscv/` + subset x86_64 |
| `startup/baremetal/x86_64/crt1` | ❌ upstream | tidak dibutuhkan — `_start` milik shim KyuzenOS |
| `libc.startup.baremetal.init/fini` (arch-neutral) | ✅ | link + panggil dari `_start` agar `.init_array` jalan |
| `app.ld` men-emit `.init_array/.fini_array` + `_end`/`__llvm_libc_heap_limit` | ❌ (app.ld saat ini discard `.gnu*`, tanpa symbol heap) | kerja linker-script app |
| Flags compile user app | ✅ | `-ffreestanding -nostdlib -mno-red-zone` sudah benar; **jangan** aktifkan `-msse` (lihat fenv) |
| Test: llvm-libc unit tests | ⚠️ | tidak bisa jalan di KyuzenOS (butuh host/lineloader); smoke test via user app di QEMU |

## 5. Primitive yang BELUM tersedia (gap kernel — dicatat, tidak diimplementasi)

1. **mmap/munmap/mprotect generik** — uheap hanya region `[0x10000000, 0x40000000)`; libc full-build (malloc Linux-style, stack guard, dlmalloc-like) tidak mungkin tanpa ini.
2. **clock_gettime presisi ns / CLOCK_MONOTONIC ns** — timer hanya ms (`timer_get_ms`); RTC hanya detik. `timespec_get`/`clock` tetap bisa dengan field ns ≈ 0-modulo-granularitas.
3. **TLS & PT_TLS** — loader tidak parse `PT_TLS`, tidak ada FS base per task (tidak ada wrfsbase/arch_prctl-equivalent). Non-issue untuk `LIBC_THREAD_MODE_SINGLE` + errno external.
4. **SSE enablement** — CR4.OSFXSR tidak pernah diset, tidak ada FXSAVE/FXRSTOR di context switch → semua entrypoint yang menyentuh MXCSR (#UD) dan SIMD-optimized paths harus dihindari; aplikasi tetap `-mno-sse -msoft-float`.
5. **futex / wait-wake ekspos ke user** — block/wake hanya internal kernel (pipe/mutex/waitpid). Tidak dibutuhkan untuk single-thread.
6. **environ/auxv** — hanya argv. Baremetal `main(0,0)` upstream bahkan tidak butuh argv.
7. **Signal, vdso, dynamic linking** — tidak ada; tidak dibutuhkan subset freestanding.

## 6. Kesimpulan

- **Freestanding subset LLVM libc di x86_64 KyuzenOS adalah feasible TANPA perubahan kernel**: baremetal mode memang didesain tanpa syscall layer — seluruh akses OS lewat 6–7 vendor hooks yang semuanya bisa dipetakan ke syscall int 0x80 yang sudah ada (exit 34, write 49, read 48, alloc 9/10/19, uptime 14, RTC 20).
- Pekerjaan port terkonsentrasi di: (a) `config/baremetal/x86_64/` baru, (b) shim userspace `_start` + hooks, (c) ekstensi `app.ld` (init_array + simbol heap). Semua di luar kernel.
- Dua risiko presisi yang harus jadi keputusan desain sebelum implementasi: **libm via x87** (extended precision) dan **granularitas waktu ms/detik**; keduanya dapat dihindari di fase awal dengan subset entrypoint string/ctype/stdio-integer/malloc.
- Jika suatu saat ingin fenv/SSE/threads/full-build, barulah kernel perlu: OSFXSR + FXSAVE/FXRSTOR, mmap generik, clock ns, TLS. Itu keputusan roadmap terpisah.
