# Audit: LLVM libc 22.1.8 (baremetal x86_64) untuk KyuzenOS

> **Status**: audit only — tidak ada implementasi, tidak ada perubahan kernel/syscall ABI.
> **Sumber**: `third_party/stdlib/llvm-project/` @ tag **`llvmorg-22.1.8`** (commit `ca7933e47`,
> diverifikasi via `cmake/Modules/LLVMVersion.cmake`: major 22, minor 1, patch 8, suffix kosong).
> **Dokumen pembanding**: `docs/design/audit-llvm-libc-freestanding.md` (audit pertama, dibuat saat
> vendored tree masih LLVM 24.0.0-dev).
> **Target**: `x86_64-pc-none-elf` → `LIBC_TARGET_OS = "baremetal"` — **identik dengan triple yang
> sudah dipakai** `Makefile` (kernel) dan `user_apps/Makefile` (host toolchain clang/ld.lld/llvm-ar 22.1.8).

---

## 1. Yang SAMA dengan LLVM 24 (temuan audit pertama tetap valid)

| Area | Bukti di 22.1.8 |
|---|---|
| Deteksi baremetal dari triple | `LLVMLibCArchitectures.cmake` (~baris 160): `none`/`unknown` → `baremetal`; arsitektur **x86_64 didukung** (`LIBC_TARGET_ARCHITECTURE_IS_X86_64`) |
| Baremetal **tanpa lapisan syscall** | `libc/src/__support/OSUtil/baremetal/` hanya `exit.cpp`, `io.cpp`, `io.h` — tidak ada `syscall.h`; dispatch `OSUtil/io.h`: `__ELF__` → baremetal |
| Exit hook vendor | `OSUtil/baremetal/exit.cpp`: `extern "C" [[noreturn]] void __llvm_libc_exit(int)` |
| Malloc baremetal = freelist heap | `src/stdlib/baremetal/malloc.cpp` → `freelist_heap->allocate()`; API `src/__support/freelist_heap.h` identik: ctor default `_end`..`__llvm_libc_heap_limit`, `FreeListHeap(span)`, `extern FreeListHeap *freelist_heap` |
| Abort = `__builtin_trap()` | `src/stdlib/baremetal/abort.cpp` → UD2 → `#UD` → `exception_handler` kernel (tanpa ubah kernel) |
| Time hooks | `src/time/baremetal/clock.cpp` → `__llvm_libc_timespec_get_active(ts)`; `timespec_get.cpp` → `__llvm_libc_timespec_get_utc(ts)` |
| errno external | `config/baremetal/config.json` → `LIBC_ERRNO_MODE_EXTERNAL` |
| Threads single | `config.json` → `LIBC_CONF_THREAD_MODE: LIBC_THREAD_MODE_SINGLE`; tidak ada kebutuhan clone/futex/TLS |
| `stdout/stdin/stderr` disediakan libc | `OSUtil/baremetal/io.cpp` mendefinisikan `FILE *stdin/stdout/stderr = &__llvm_libc_*_cookie` |
| Startup arch-neutral | `startup/baremetal/init.cpp`/`fini.cpp` menjalankan `__preinit_array`/`__init_array`/`__fini_array` |
| crt1 x86_64 tidak ada upstream | `startup/baremetal/CMakeLists.txt:54–58`: hanya `aarch64`, `arm`; arsitektur lain → warning *"Cannot build 'crt1.o' for x86_64 yet."* |
| Config dir baremetal per-arch | `libc/config/baremetal/` hanya berisi `aarch64`, `arm`, `riscv` (+ `config.json`) |
| `__stack_chk_fail` disediakan libc | `entrypoints.txt` baris 6: `libc.src.compiler.__stack_chk_fail` (bukan tanggungan vendor) |
| `__cxa_atexit` ikut libc | `src/stdlib/atexit.cpp` mendefinisikan `__cxa_atexit` + `__cxa_finalize`; entrypoint `libc.src.stdlib.atexit` ada di baremetal |

## 2. yang BERBEDA dari LLVM 24

| Aspek | LLVM 24 (audit lama) | LLVM 22.1.8 (terverifikasi) |
|---|---|---|
| **stdio baremetal, file** | `src/stdio/baremetal/` punya `stdout.cpp/stderr.cpp/stdin.cpp`, `fclose/fflush/fseek/fseeko/ftell/ftello/ungetc`, `printf_modular.cpp` | **TIDAK ADA** — `stdin/stdout/stderr` didefinisikan langsung di `OSUtil/baremetal/io.cpp`; tanpa fclose/fflush/fseek/ftell/ungetc |
| **Permukaan stdio (entrypoints riscv)** | ±80 entrypoint stdio | Lebih kecil: printf/puts/putchar/putc/fputs/fwrite/fgets/fgetc/fread/scanf/… + `snprintf/sprintf/sscanf/asprintf/vasprintf` (generik); **tanpa** `stdio.stdout/stderr/stdin`, tanpa `fopencookie`, tanpa `remove` hook FILE — tapi `stdio.remove.cpp` baremetal ada |
| **stdlib baremetal** | `abort_utils.h` (inline) + `malloc_usable_size.cpp` | `abort.cpp` (file terpisah); **tidak ada** `malloc_usable_size` di entrypoints |
| **config.json** | + `LIBC_CONF_PRINTF_DISABLE_WIDE`, `LIBC_COPT_PRINTF_DISABLE_BITINT`, `LIBC_CONF_SCANF_DISABLE_FLOAT/INDEX_MODE`, `LIBC_CONF_STRTOFLOAT_DISABLE_EISEL_LEMIRE/CLINGER`, `LIBC_CONF_CTYPE_SMALLER_ASCII`, `LIBC_CONF_ENABLE_STRONG_STACK_PROTECTOR:false`, math `NO_EXCEPT`+`ASSUME_ROUND_NEAREST_ONLY` | Tanpa semua opsi tambahan itu. Stack protector: opsi yang ada `LIBC_CONF_ENABLE_STACK_PROTECTOR` (`LLVMLibCCompileOptionRules.cmake:220`, default mati) — tidak perlu dipatikan manual |
| **`LIBC_COPT_FREESTANDING_MALLOC`** | Di-cache hasil audit 24 sebagai opsi | **Tidak ada di 22** — malloc baremetal *hanya* jalur `src/stdlib/baremetal/` (freelist). Konsekuensi: memilih arena memori dilakukan lewat simbol linker atau instansiasi `freelist_heap` sendiri, bukan option cmake |
| **fenv/math opt** | `LIBC_MATH_NO_EXCEPT` + `ASSUME_ROUND_NEAREST_ONLY` | 22 hanya `SKIP_ACCURATE_PASS\|SMALL_TABLES\|NO_ERRNO\|INTERMEDIATE_COMP_IN_FLOAT` — sedikit lebih konservatif; tidak mengubah kesimpulan (fenv tetap ❌ di KyuzenOS, lihat §6) |
| **Build doc** | `libc/docs/full_cross_build.md` | `libc/docs/full_cross_build.rst` |
| **Arch list cmake** | + `spirv` (32-bit) | `spirv64` saja |

**Koreksi terhadap audit LLVM 24**: di dokumen lama hook errno ditulis "definisi global `int` di shim".
Secara mekanis (berlaku di 22.1.8 dan juga 24): external mode menuntut vendor mendefinisikan
`extern "C" int *__llvm_libc_errno(void)` (`src/__support/libc_errno.h:70–77`) — fungsi yang
mengembalikan pointer ke variabel errno milik embedder, bukan sekadar variabel global.

## 3. Yang HILANG di LLVM 22 (relatif terhadap 24 / perlu jadi perhatian)

1. **`config/baremetal/x86_64/` tetap tidak ada** (hanya `aarch64`, `arm`, `riscv`) — harus dibuat sendiri (sama seperti 24).
2. **`startup/baremetal/x86_64/crt1` tidak ada** (hanya aarch64/arm) — `_start` tetap tugas shim KyuzenOS (sama seperti 24).
3. **stdio baremetal lebih tipis**: tidak ada `fclose/fflush/fseek/ftell/ungetc`, tidak ada entrypoint `stdio.stdout/stderr/stdin`/`fopencookie` — semua stdio berorientasi terminal saja (cukup untuk KyuzenOS fase awal; FILE* custom tidak mungkin di 22).
4. **`malloc_usable_size` tidak tersedia** di entrypoints baremetal 22.
5. **Tidak ada opsi config** `DISABLE_WIDE`, bitint-printf, `SMALLER_ASCII`, strtofloat tuning — printf/scanf 22 sedikit lebih besar, tapi baremetal config sudah mematikan float printf.
6. **`setjmp/longjmp`** sengaja dikomentari di entrypoints baremetal (meski `src/setjmp/x86_64/` tersedia di tree) — biarkan begitu untuk fase awal.

Yang 22 punya tapi tak tercatat di audit 24 (nilai tambah):
- `libc.src.stdio.fopencookie` **generik** tersedia di tree (`src/stdio/fopencookie.cpp`) tapi **tidak masuk** daftar entrypoints baremetal riscv — bisa ditambahkan sendiri di entrypoints.txt x86_64 jika suatu saat perlu FILE custom.
- `__cxa_atexit`/`__cxa_finalize` datang gratis lewat `libc.src.stdlib.atexit` (penting untuk C++ nanti).

## 4. File yang perlu DIBUAT untuk x86_64 KyuzenOS (saat implementasi nanti)

| File | Isi | Sisi |
|---|---|---|
| `libc/config/baremetal/x86_64/entrypoints.txt` | subset riscv (string/ctype/stdbit/errno/locale/stdlib-int + malloc/exit/stdio/time sesuai fase) | LLVM config |
| `libc/config/baremetal/x86_64/headers.txt` | salin riscv (assert…wctype; `setjmp`, `fenv` tunda) | LLVM config |
| Shim userspace (mis. `user_apps/libc_port/`): `_start` | asm entry: teruskan `RDI=argc, RSI=argv` (SysV-compliant, `RSP%16==0`), panggil `__libc_init_array()` bila di-link, lalu `main` | KyuzenOS |
| Shim: `__llvm_libc_exit` | → int 0x80 #34 (`sys_exit_code`) | KyuzenOS |
| Shim: struct `__llvm_libc_stdio_cookie` + 3 objek cookie + `__llvm_libc_stdio_read/write` | → fd 0/1/2 via int 0x80 #48/#49 | KyuzenOS |
| Shim: `__llvm_libc_errno` | fungsi `int*` yang menunjuk variabel errno global app | KyuzenOS |
| Shim: `__llvm_libc_timespec_get_utc/active` | → #20 RTC (detik → epoch perlu days-from-civil) + #14 uptime ms | KyuzenOS |
| `user_apps/app.ld` (ekstensi) | `.init_array/.fini_array` + simbol `__init_array_start/end`, `__preinit_array_*`, `__fini_array_*`; `_end` + `__llvm_libc_heap_limit` (bila freelist heap statis) | KyuzenOS (linker script app, bukan kernel) |
| (opsional) instansiasi `freelist_heap` | `FreeListHeap(span)` di atas region `sys_alloc`(9) bila tidak mau heap statis linker | KyuzenOS |

Tidak ada file LLVM yang perlu dimodifikasi isinya — hanya direktori config baru.

## 5. Hook yang WAJIB disediakan KyuzenOS (ringkasan §4, tanda tangan persis dari 22.1.8)

1. `void __llvm_libc_exit(int status)` — noreturn
2. `struct __llvm_libc_stdio_cookie` (tipe, isi bebas) + objek `__llvm_libc_stdin_cookie`, `__llvm_libc_stdout_cookie`, `__llvm_libc_stderr_cookie` (wajib ada walapi tak dipakai — komentar `io.h`)
3. `ssize_t __llvm_libc_stdio_read(void *cookie, char *buf, size_t size)`
4. `ssize_t __llvm_libc_stdio_write(void *cookie, const char *buf, size_t size)`
5. `int *__llvm_libc_errno(void)` (errno external)
6. `bool __llvm_libc_timespec_get_utc(struct timespec *ts)`
7. `bool __llvm_libc_timespec_get_active(struct timespec *ts)`
8. Simbol linker: `__init_array_start/end`, `__preinit_array_start/end`, `__fini_array_start/end`, `_end`, `__llvm_libc_heap_limit`

Total: **4 fungsi + 3 objek + 1 tipe + 6–8 simbol linker + `_start`** — semua ke syscall/ELF yang sudah ada.

## 6. Apakah syscall ABI KyuzenOS saat ini cukup? — **Ya, untuk subset freestanding**

| Kebutuhan 22.1.8 | Syscall KyuzenOS | Status |
|---|---|---|
| exit | #34 | ✅ |
| stdio read/write | #48/#49 pada fd 0/1/2 (dipasang kernel per task) | ✅ |
| malloc arena | #9/#10/#19 (uheap region page-granular) atau heap statis | ✅ |
| monotonic | #14 uptime (ms) | ⚠️ granularitas ms |
| wall clock | #20 RTC (detik) | ⚠️ presisi detik |
| entry ABI | RDI=argc, RSI=argv, IRETQ → RSP%16==0, ELF statis (loader hanya parse PT_LOAD, BSS di-zero) | ✅ |
| sleep (tidak masuk subset) | #46 sleep(ms) | ⚠️ tidak masuk entrypoints baremetal |
| trap (abort) | `__builtin_trap()` → #UD → `exception_handler` yang sudah ada | ✅ tanpa ubah kernel |

ABI int 0x80 (`RAX=num; RBX/RCX/RDX/RSI/RDI` = arg1..5, return RAX) memang bukan konvensi Linux,
tapi di jalur baremetal **tidak relevan** — semua hook di §5 adalah fungsi C biasa yang di dalamnya
memanggil int 0x80 dengan konvensi KyuzenOS. `syscall.h` Linux tidak dipakai sama sekali.

## 7. Apakah perlu perubahan kernel? — **TIDAK**

Fase subset (string/ctype/stdlib/errno/malloc/stdio/time) nol perubahan kernel:
- stdio cukup fd 0/1/2; malloc cukup uheap; exit/trap cukup #34/#UD.
- Single-thread (`LIBC_THREAD_MODE_SINGLE`) + errno external → tidak butuh TLS, PT_TLS, FS base, futex, clone.
- SSE/OSFXSR tetap tidak diset kernel → **tetap** `-mno-sse -msoft-float`, hindari fenv/MXCSR dan printf float (config baremetal 22 memang mematikan float printf).

## 8. Apakah app linker script perlu perubahan? — **Ya**

`user_apps/app.ld` saat ini: hanya `PT_LOAD` text/data, `ENTRY(main)`, discard `.comment/.note*/.gnu*/.eh_frame*/.debug*` — **tidak men-emit** `.init_array/.fini_array` maupun simbol heap. Perlu:
1. Section + simbol `__init_array_start/end`, `__preinit_array_*`, `__fini_array_start/end` (agar `__libc_init_array()` jalan — ini pengganti "loader tidak menjalankan .init_array", konsisten dengan aturan widget "tanpa global ctor" karena inisialisasi jalan dari `_start` shim, bukan loader kernel).
2. `_end` + `__llvm_libc_heap_limit` bila memakai freelist heap statis (atau instansiasi `FreeListHeap(span)` atas region `sys_alloc` — pilih salah satu; simbol linker = jalur paling murah).
3. `.note*`/`.gnu*` tetap di-discard — tidak masalah karena tidak ada PT_TLS/PT_DYNAMIC di subset.

## 9. Dependency C++ (nanti, bila masuk C++) — 22.1.8

- **C++ subset tanpa exceptions/RTTI** (kondisi KyuzenOS sekarang: `-fno-exceptions -fno-rtti`, aturan widget "operator new/delete + `__cxa_pure_virtual` hanya di `runtime.cpp`", `.init_array` tak pernah jalan): cukup **libc** saja. `libc.src.stdlib.atexit` menyediakan `__cxa_atexit`/`__cxa_finalize`; `__cxa_pure_virtual` sudah ada di runtime widget. `libunwind` tidak diperlukan tanpa unwinding.
- **`libc++`** (22.1.8 ada di tree): butuh header + `libc++abi`; di baremetal dibangun dengan threads mati. Tidak ada cache baremetal bawaan di `libcxx/cmake/caches` (22.1.8) — konfigurasi manual (`LIBCXX_ENABLE_THREADS=OFF`, `LIBCXX_ENABLE_EXCEPTIONS=OFF`, abi-library `none`/libc++abi-static) bila nanti diperlukan.
- **`libc++abi`** (ada di tree): diperlukan bila C++ butuh personality/demangle/`__cxa_*` lengkap (exceptions, static dtors melampaui libc atexit).
- **`libunwind`** (ada di tree): hanya bila exceptions atau backtrace unwinding.
- **compiler-rt builtins**: kandidat satu-satunya kebutuhan nyata adalah helper aritmetika 128-bit (`__udivti3` dsb.) bila kode app/libc memicu pembagian `__int128`; subset C string/stdio/stdlib umumnya tidak memicunya. printf float dimatikan config baremetal, sehingga jalur soft-float tidak menarik builtins FP.
- Urutan ketergantungan bila C++ penuh: compiler-rt builtins → libunwind → libc++abi → libc++.

## 10. Urutan implementasi paling kecil / rendah risiko (setelah tahap audit ini)

1. **Fase 0 — config-only**: buat `libc/config/baremetal/x86_64/{entrypoints,headers}.txt` subset *murni* (string.h/strings.h/ctype/stdbit/errno/locale/inttypes; **tanpa** stdio/malloc/time) → cross-build statis `.a` mengikuti `libc/docs/full_cross_build.rst`; smoke test link-only di host. Risiko: nol terhadap kernel.
2. **Fase 1 — exit + malloc**: shim `_start` + `__llvm_libc_exit` (#34) + freelist heap (simbol linker atau region `sys_alloc`) + errno external; `app.ld` dapat ekstensi init_array + heap symbols. Smoke test QEMU: malloc/strlen/exit.
3. **Fase 2 — stdio**: cookie struct + `__llvm_libc_stdio_read/write` → #48/#49; aktifkan printf/puts/getchar. Smoke test QEMU: printf hello + argv.
4. **Fase 3 — time**: `__llvm_libc_timespec_get_utc/active` dari #20/#14; `clock`/`timespec_get`.
5. **Fase 4 (keputusan terpisah)**: libm/fenv (risiko x87 presisi + MXCSR #UD tanpa OSFXSR — lihat audit 24 §5), C++ runtime (compiler-rt/libunwind/libc++abi), atau ekspansi config ke fd POSIX-ish (47–51, 74–79 sudah tersedia bila diperlukan).

Setiap fase bisa diverifikasi dengan satu user ELF smoke-test di QEMU (`make run-wd`), tanpa pernah menyentuh `kernel/`.

## 11. Kesimpulan

- **LLVM 22.1.8 cukup** untuk subset freestanding KyuzenOS; hook vendor (§5) identik dengan 24, permukaan stdio sedikit lebih kecil (tanpa fclose/fflush/fseek/ungetc/malloc_usable_size — tidak menghalangi fase awal).
- **Syscall ABI sekarang cukup**; **tidak ada perubahan kernel**; **app.ld perlu ekstensi** (init/fini array + simbol heap).
- Pekerjaan yang tersisa sebelum implementasi murni keputusan: memilih sumber arena malloc (simbol linker vs `sys_alloc`) dan apakah libm masuk fase awal (rekomendasi: tidak).

---

## 12. Phase 0 implementation status

**Status: SELESAI & TERVERIFIKASI** (clean build + link smoke test).
Ruang lingkup: hanya membuktikan bahwa konfigurasi LLVM libc freestanding x86_64 dapat
dibuild dan archive-nya dapat di-link. Tanpa vendor hook, tanpa kernel change, tanpa `_start`, tanpa malloc/stdio/time/C++.

### 12.1 File yang dibuat

| File | Isi |
|---|---|
| `third_party/stdlib/llvm-project/libc/config/baremetal/x86_64/entrypoints.txt` | **162 entrypoint** pure; `TARGET_LIBM_ENTRYPOINTS` kosong; menutup dengan `set(TARGET_LLVMLIBC_ENTRYPOINTS ...)` |
| `third_party/stdlib/llvm-project/libc/config/baremetal/x86_64/headers.txt` | 8 public header: ctype, errno, inttypes, locale, stdbit, stdint, string, strings |
| `third_party/stdlib/llvm-project/libc/cmake/caches/x86_64-pc-none-elf.cmake` | cache resmi-style: `RUNTIMES_TARGET_TRIPLE=x86_64-pc-none-elf` + flag `-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float`, lalu `include(baremetal_common.cmake)` |
| `tools/libc-phase0/smoke.c` | smoke test link: `memcpy/memset/memcmp/strlen/strcmp/isalpha/isdigit` (indirection volatile agar codegen tidak meng-inline) |
| `Makefile` | target opt-in `libc-phase0` (+ `libc-clean`); tidak mengubah target default |

Tidak ada file LLVM libc yang diubah isinya; `libc/docs/configure.rst` yang diregenerasi cmake sudah dikembalikan (`git checkout`).

### 12.2 Entrypoint yang dipilih (per modul)

| Modul | Jumlah | Catatan |
|---|---|---|
| ctype (+`_l`) | 30 | murni |
| locale | 6 | di 22.1.8 semuanya bebas-alokasi (`freelocale` no-op, `newlocale` mengembalikan `c_locale` statis) → tidak menarik malloc |
| string | 39 | `strdup`/`strndup` **sengaja dikecualikan** (memanggil malloc) |
| strings | 12 | murni |
| inttypes | 4 | murni (compile-time; `strtoimax/strtoumax` hanya butuh hook errno saat dipakai) |
| stdbit | 70 | murni |
| errno | 1 | objek ikut di archive; hanya butuh `__llvm_libc_errno()` bila dirujuk |
| **libm** | 0 | `TARGET_LIBM_ENTRYPOINTS` kosong → `libm.a` tidak dibangun (sesuai desain) |

Tidak ada entrypoint stdio, malloc, time, threads, fenv, setjmp, filesystem, networking, process, maupun syscall.

### 12.3 Command / build target

Prasyarat host (terverifikasi di mesin ini): clang/clang++/ld.lld/llvm-*/make/sh dari msys2,
cmake 4.2.3, **python + pyyaml 6.0.3** (hdrgen). `ninja` tidak dipakai → generator **Unix Makefiles**.

```sh
# PATH: msys2 usr/bin (make, sh) + clang64/bin (clang, ld.lld, llvm-nm)
rm -rf build/libc            # clean build
make libc-phase0 \
  CMAKE="/c/Program Files/CMake/bin/cmake.exe" \
  LIBC_PYTHON_EXE=E:/Tools/Language/Python/python.exe
```

Perintah cmake yang dijalankan target ini (standalone cross build, `libc/docs/full_cross_build.rst`):

```sh
cmake -S third_party/stdlib/llvm-project/runtimes -B build/libc/cmake \
      -G "Unix Makefiles" \
      -C third_party/stdlib/llvm-project/libc/cmake/caches/x86_64-pc-none-elf.cmake \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=<python+pyyaml>
cmake --build build/libc/cmake --target libc -- -j8
```

(`ninja libc libm` dari dokumen upstream setara dengan `--target libc` di sini; `libm` tidak dibangun karena entrypoint libm kosong.)

### 12.4 Artifact yang dihasilkan

| Artifact | Ukuran / isi |
|---|---|
| `build/libc/x86_64-pc-none-elf/lib/libc.a` | 236.338 byte, **164 member** (162 entrypoint + objek internal pendukung) |
| `build/libc/cmake/libc/include/*.h` | header hasil hdrgen: `ctype.h errno.h inttypes.h locale.h stdbit.h stdint.h string.h strings.h` (+ `float.h limits.h wchar.h __llvm-libc-common.h`, `llvm-libc-macros/`, `llvm-libc-types/` sebagai dependensi generator) |
| `build/libc/smoke.o`, `build/libc/smoke.elf` | 2.744 / 5.688 byte; ELF64 EXEC x86-64, entry `phase0_entry` @ `0x2012b0` |
| `build/libc-build.log` | log build lengkap (untuk audit ulang) |

Semua di bawah `build/` (gitignored) dan ikut terhapus oleh `make clean`.

### 12.5 Hasil smoke test

```
ld.lld -m elf_x86_64 -nostdlib --entry=phase0_entry -o build/libc/smoke.elf \
       build/libc/smoke.o build/libc/x86_64-pc-none-elf/lib/libc.a
[libc] smoke OK: 7/7 simbol pure dari libc.a, 0 undefined symbol
```

Bukti tambahan (bukan sekadar "link tidak error"):

- `llvm-nm --undefined-only smoke.elf` → **kosong**; `llvm-nm --defined-only` → 18 simbol text, termasuk 7 simbol target.
- Disassembly `phase0_entry` menunjukkan `callq` nyata ke `strlen`/`memset`/`memcpy`/`strcmp`/`memcmp`/`isalpha`/`isdigit` (indirection volatile mencegah inlining/folding, jadi archive benar-benar dieksekusi jalur link).
- Di dalam archive simbol adalah `FUNC GLOBAL HIDDEN` (mis. `strlen`); ld.lld men-lokalkan simbol hidden pada executable statis, sehingga `llvm-nm` menampilkannya sebagai `t`. Karena itu assertion Makefile menerima `[TtWw]`.
- `libc/CMakeLists.txt` menghasilkan `TARGET_ENTRYPOINT_NAME_LIST` dari `TARGET_LLVMLIBC_ENTRYPOINTS`; config yang tidak men-set variabel ini membuat **semua** entrypoint jadi dummy target (error `Target ... of type UTILITY may not be linked`). Ini satu-satunya jebakan konfigurasi yang ditemukan.

### 12.6 Verifikasi build existing (tidak boleh berubah)

| Perintah | Hasil |
|---|---|
| `rm -rf build/libc && make libc-phase0 ...` | exit 0; **0 compiler error, 0 linker error** |
| `make` (kernel) | exit 0; `build/bin/myos.bin` ada (484.504 byte) |
| `make apps` (user_apps) | exit 0; 19 ELF di `build/apps/` |

Peringatan CMake yang tersisa (2, keduanya benign & tidak muncul lagi sebagai error):

1. `GetHostTriple.cmake:46: unable to determine host target triple` — host Windows; tidak relevan untuk cross build.
2. `startup/baremetal/CMakeLists.txt:57: Cannot build 'crt1.o' for x86_64 yet.` — **sesuai audit §1** (crt1 x86_64 memang tidak ada upstream; `_start` adalah tugas Phase 1).

### 12.7 Known limitations Phase 0

1. **Semua vendor hook masih absen** — `__llvm_libc_exit`, `__llvm_libc_stdio_read/write` + 3 cookie, `__llvm_libc_errno`, `__llvm_libc_timespec_get_utc/active` belum ada. Archive tetap linkable selama objek yang merujuknya tidak dipakai; entrypoint yang menyentuh errno (`strtoimax/strtoumax`, `strerror*` jalur errno) akan butuh `__llvm_libc_errno()` saat dirujuk.
2. **Tidak ada `libm.a`** dan tidak ada entrypoint stdio/malloc/time/threads/fenv/setjmp — sesuai scope.
3. **Hanya uji link, bukan uji jalan**: host adalah Windows sehingga ELF `x86_64-pc-none-elf` tidak bisa dieksekusi; validasi runtime baru mungkin setelah ada aplikasi KyuzenOS yang memuat archive ini (Phase 1+).
4. **`libc/docs/configure.rst` diregenerasi setiap cmake configure** (side effect upstream di dalam vendored tree, bukan ignored). Bila ingin tree bersih: `git -C third_party/stdlib/llvm-project checkout -- libc/docs/configure.rst`.
5. **`make clean` menghapus `build/libc`** sehingga perlu configure ulang (~50 detik) pada `make libc-phase0` berikutnya.
6. Archive dibangun dengan `-mno-sse -mno-sse2 -mno-mmx -msoft-float` mengikuti kebijakan kernel KyuzenOS (tidak ada `CR4.OSFXSR`/FXSAVE): penting agar kode libc tidak memakai SSE yang akan #UD. Varian SIMD x86_64 (SSE2/AVX) tidak masuk entrypoints sehingga tidak dibangun.
7. `TARGET_LIBM_ENTRYPOINTS` kosong → konsumen harus tahu bahwa `libm.a` tidak akan ada sampai Phase libm.

### 12.8 Batas eksplisit tahap ini

Belum dan sengaja belum dikerjakan: `_start`/crt, vendor hook apa pun, malloc/heap, stdio, time, fenv, C++/libc++abi/libunwind, integrasi ke aplikasi Kyuzen (`app.ld`, `user_apps/Makefile`), dan Phase 1 berikutnya.

---

## 13. Phase 1 implementation status (exit + errno + malloc) — SELESAI

> Status: diimplementasi, di-link, **dan dijalankan di QEMU** (bukan lagi uji link).
> Tidak ada perubahan `kernel/`, syscall ABI, atau app Kyuzen lain. `limine.conf` repo
> tidak diubah. Sumber LLVM tetap `llvmorg-22.1.8` (hanya 2 file config baru + 1 cache
> build, sama seperti Phase 0).

### 13.1 Desain allocator yang dipilih (audit ulang FreeListHeap 22.1.8)

Fakta dari source 22.1.8 (`src/__support/freelist_heap.h/.cpp`):

- `FreeListHeap` bekerja di atas **satu region contiguous** dengan batas atas tetap;
  ctor default menunjuk `_end`..`__llvm_libc_heap_limit` dan **tidak ada hook"minta
  memori tambahan"** → tidak mungkin memetakan satu `sys_alloc(9)` per `malloc()`.
- Inisialisasi heap **lazy** (`allocate_impl` memanggil `init()` saat pertama), jadi
  tidak ada `.init_array` yang dibutuhkan untuk heap.
- libc mendefinisikan instance bawaan di `freelist_heap.cpp.obj`:
  `static LIBC_CONSTINIT FreeListHeap freelist_heap_symbols; FreeListHeap *freelist_heap = &freelist_heap_symbols;`
  — inilah satu-satunya pemakai simbol `_end`/`__llvm_libc_heap_limit` di archive.

Keputusan (port layer, bukan modifikasi algoritma allocator LLVM):

```
LLVM libc malloc/calloc/realloc/free/aligned_alloc   (src/stdlib/baremetal/*.cpp)
        ↓ freelist_heap->allocate() ...              (algoritma freelist LLVM 22.1.8)
__llvm_libc_22_1_8_::freelist_heap                   (definisi kuat di port layer)
        ↓ FreeListHeap(span) di atas SATU region     (placement new dari CPP/new.h)
sys_alloc(9) 1 MiB  →  Kyuzen uheap (page-granular)
```

- `freelist_heap` didefinisikan ulang (kuat) di object port → member archive
  `freelist_heap.cpp.obj` **tidak pernah ditarik linker** → simbol `_end`/
  `__llvm_libc_heap_limit` tidak dibutuhkan sama sekali (diverifikasi: 0 undefined
  `_end` pada link app). Tidak ada arena statis di citra ELF, tidak ada allocator kedua.
- Arena: `sys_alloc(9)` sebesar `KYUZEN_LIBC_ARENA_BYTES` (default 1 MiB, bisa di-
  override saat kompilasi). uheap mengembalikan awal halaman, jadi syarat alignment
  `Block::MIN_ALIGN` terpenuhi.
- Bila `sys_alloc` gagal: port memanggil `__llvm_libc_exit(70)` (`KZ_EXIT_ARENA_FAILED`)
  — bukan allocator cadangan kedua, sesuai batasan desain.
- Ruang alamat uheap jauh lebih besar (`UHEAP_BASE 0x10000000 .. UHEAP_END 0x40000000`,
  plafon satu alokasi 64 MiB); plafon arena adalah konsekuensi API FreeListHeap, bukan
  keterbatasan uheap.

### 13.2 Syscall mapping yang dipakai port layer

| Hook / kebutuhan libc | Syscall KyuzenOS | Catatan |
|---|---|---|
| `__llvm_libc_exit(int)` | `int 0x80` #34 (`RAX=34, RBX=status`) | dipakai `_Exit`, `exit`, `abort`→trap, dan `_start` |
| `__llvm_libc_errno()` | — (fungsi biasa, state global per proses) | `LIBC_ERRNO_MODE_EXTERNAL`; tanpa TLS sesuai Phase 1 |
| backing `freelist_heap` | `int 0x80` #9 (`RBX=ukuran` → alamat) | satu region untuk seluruh heap, bukan per malloc |
| (khusus smoke test, bukan port) | #78 fork, #69 waitpid, #49 write(fd 1) | hanya di `tools/libc-phase1/libc_phase1.c` untuk pembuktian |

### 13.3 Files changed

| File | Jenis | Isi |
|---|---|---|
| `libc/config/baremetal/x86_64/entrypoints.txt` | dimodifikasi (dari Phase 0) | +8: `abort`, `_Exit`, `calloc`, `exit`, `free`, `malloc`, `realloc`, `atexit` (`atexit` dipaksa karena `exit.cpp` memanggil `__cxa_finalize`; `exit_deps` baremetal tidak menyertakan `.atexit`) |
| `libc/config/baremetal/x86_64/headers.txt` | dimodifikasi | +`libc.include.stdlib` (hdrgen kini menghasilkan `stdlib.h`) |
| `libs/libc-port/src/kyuzen_libc_port.cpp` | baru | port layer: `__llvm_libc_exit`, `__llvm_libc_errno`, `freelist_heap` + `kyuzen_libc_heap_init`, `_start` + `kyuzen_libc_start_c` |
| `tools/libc-phase1/libc_phase1.c` | baru | smoke test app (malloc/calloc/realloc/free, pola, errno, kasus gagal, exit status via waitpid) |
| `tools/libc-phase1/libc_app.ld` | baru | linker script app: `ENTRY(_start)`, 2 PT_LOAD, `.init_array` **tidak** dimasukkan (tidak dibutuhkan) |
| `tools/libc-phase1/run-qemu.sh` | baru | automasi QEMU: disk uji segar, keystroke lewat monitor HMP berbasis marker serial, watchdog + hard kill |
| `Makefile` | aditif | rules `libc-phase1` (port + app + assert link), `libc-phase1-qemu`, generator conf ISO; variabel `LIMINE_CONF ?= limine.conf` pada rule ISO (default perilaku lama) |
| `docs/design/audit-llvm-libc-22-freestanding.md` | modifikasi | bagian §13 ini |

`limine.conf` repo **tidak berubah**: module `libc_phase1.elf` hanya masuk ISO uji lewat
conf hasil generate (`build/libc/iso/limine.conf` = repo conf + 1 blok module) yang
disuntik dengan `make boot_image.iso LIMINE_CONF=build/libc/iso/limine.conf`. Alasannya:
Limine gagal memuat module yang filenya tidak ada, jadi entri permanen di repo conf akan
mematahkan ISO build normal saat artifact Phase 1 belum ada.

### 13.4 Build & artifact

```
export PATH="/e/Tools/msys2/usr/bin:/e/Tools/msys2/clang64/bin:$PATH"
make libc-phase0 \                       # rebuild libc.a (entrypoints berubah) + smoke Phase 0
  CMAKE="/c/Program Files/CMake/bin/cmake.exe" LIBC_PYTHON_EXE=E:/Tools/Language/Python/python.exe
make libc-phase1                          # port object + link app (assert: entry _start, 0 undefined)
make libc-phase1-qemu                     # ISO uji + QEMU otomatis
```

| Artifact | Ukuran | Catatan |
|---|---|---|
| `build/libc/x86_64-pc-none-elf/lib/libc.a` | 278.138 B, 178 member | +16 member vs Phase 0 (stdlib malloc/exit/atexit + internal freelist/freetrie) |
| `build/libc/x86_64-pc-none-elf/bin/libc_phase1.elf` | 19.984 B | ELF64 EXEC, entry `_start` @ `0x40007c0`, 2 PT_LOAD, **tanpa** `.init_array`, **tanpa** instruksi SSE/x87 (dicek `llvm-objdump`) |
| `build/libc/port/kyuzen_libc_port.o` | 2.072 B | mendefinisikan `_start`, `__llvm_libc_exit`, `__llvm_libc_errno`, `freelist_heap` (mangled `__llvm_libc_22_1_8_`) |
| `build/boot_image.iso` (uji) | 13.058.048 B | memuat `/apps/libc_phase1.elf` sebagai module Limine |

Bukti desain: `llvm-nm --print-file-name --undefined-only libc.a` menunjukkan `_end`/
`__llvm_libc_heap_limit` hanya di `freelist_heap.cpp.obj` — dan member itu **tidak ikut**
ter-link (app Phase 1 link dengan 0 undefined symbol).

### 13.5 Hasil smoke test QEMU (bukti serial COM1)

```
[KZFS4] Disk belum terformat V4 — memformat...
[KZFS4] Disk diformat KyuzenFS V4 (extent-based)
  [mod] 45/45 /apps/libc_phase1.elf 19984B OK
root@kyuzen> start libc_phase1
start: libc_phase1.elf (task 2)
[phase1] start
[phase1] ok malloc(4096)+pola
[phase1] ok realloc(8192)+data-utuh
[phase1] ok calloc(64,32)=zero
[phase1] ok errno (rw + ERANGE dari libc)
[phase1] ok permintaan melebihi arena = NULL
[phase1] ok free + reuse
[phase1] ok exit(7) anak terbaca waitpid (syscall #34)
[phase1] PASS
```

Interpretasi per poin:

1. **malloc + tulis/baca pola** — heap LLVM hidup di atas region `sys_alloc(9)`.
2. **realloc 4 KiB→8 KiB** — `FreeListHeap::realloc` (alokasi baru + copy + free lama); data lama utuh.
3. **calloc zero-init** — 64×32 byte semuanya 0.
4. **errno** — tulis/baca lewat hook `__llvm_libc_errno`, dan `strtoimax` benar-benar menyetel `ERANGE` (jalur `libc_errno` internal libc → hook).
5. **kasus gagal** — `malloc(8 MiB)` > arena 1 MiB → `NULL` (upstream baremetal tidak menyetel errno saat gagal).
6. **free + reuse** — free lalu malloc lagi berhasil (free store benar).
7. **exit status** — anak `_Exit(7)` → `__llvm_libc_exit(7)` → `int 0x80` #34; parent `waitpid` membaca `7`. Ini bukti nyata status keluar datang dari syscall #34, bukan return `main` biasa.

Catatan lingkungan uji (bukan scope Phase 1): disk uji adalah **image nol mentah** —
kernel memformat sendiri (`kfs_init`). Dengan image hasil `mkfs` host, mount OK tetapi
`kfs_create_file("users.sys")` pertama-boot gagal sehingga login DENIED (temuan
pra-ada, tidak disentuh karena melarang perubahan kernel/FS).

### 13.6 Verifikasi regresi (build existing tidak boleh berubah)

| Perintah | Hasil |
|---|---|
| `make` (kernel) | exit 0 |
| `make apps` | exit 0, 19 ELF |
| `make libc-phase0` (clean archive + smoke) | exit 0, `smoke OK: 7/7` |
| `make boot_image.iso` (default, tanpa `LIMINE_CONF`) | exit 0; `build/iso_root/limine.conf` identik dengan repo conf; **0** kemunculan `libc_phase1` di conf |
| `git diff limine.conf` | kosong (file tidak berubah) |
| `git -C third_party/stdlib/llvm-project status` | hanya 2 entri baru (`config/baremetal/x86_64/`, `cmake/caches/x86_64-pc-none-elf.cmake`) |

### 13.7 Known limitations Phase 1

1. **Arena malloc berukuran tetap** (default 1 MiB, konstanta `KYUZEN_LIBC_ARENA_BYTES`): `FreeListHeap` 22.1.8 tidak punya grow hook. Permintaan di atas arena → `NULL`. Menambah kapasitas = menaikkan konstanta (biaya: halaman uheap ditahan sejak `_start`).
2. **malloc/calloc/realloc gagal tidak menyetel `errno`** — perilaku upstream baremetal 22.1.8 (tidak ada jalur errno di `baremetal/malloc.cpp`).
3. **errno = satu global per proses** (tanpa TLS) — disengaja Phase 1; data race tidak relevan karena model Kyuzen single-thread per proses.
4. **`.init_array`/`.fini_array` belum didukung port** (`libc_app.ld` sengaja tidak menaruhnya): subset Phase 1 tidak punya global constructor (heap & errno const-init). Fase yang butuh C++/global ctor harus menambah section + pemanggil `__libc_init_array` dari `_start`.
5. **`aligned_alloc` dideklarasikan `stdlib.h` tetapi tidak diaktifkan di entrypoints** → memakainya = link error; tambahkan `libc.src.stdlib.aligned_alloc` bila diperlukan (implementasinya satu baris di atas `freelist_heap`, sudah tersedia upstream).
6. **`abort()` = `__builtin_trap()`** → #UD → panic handler Kyuzen (perilaku upstream baremetal; bukan clean exit).
7. **`exit()` menjalankan `__cxa_finalize`** (atexit callbacks) tapi tidak ada TLS/thread cleanup — konsisten dengan `LIBC_THREAD_MODE_SINGLE` upstream.
8. **Automasi QEMU bergantung pada marker serial** (`Password Root Baru : `, `Username : `, `@kyuzen>`) dan mirror TTY→COM1; ada watchdog + hard kill agar tidak menggantung.
9. Port layer meng-include header internal libc (`src/__support/freelist_heap.h`, `CPP/new.h`) — kontrak tidak stabil antar versi LLVM; port harus direview ulang saat upgrade LLVM.

### 13.8 Batas eksplisit tahap ini

Belum dan sengaja belum dikerjakan: stdio (`__llvm_libc_stdio_read/write` + cookie), time (`__llvm_libc_timespec_get_*`), fenv/libm, threads/TLS, dynamic linking, C++/libc++abi/libunwind, integrasi ke `user_apps/Makefile` global, dan perubahan apa pun di `kernel/`.

---

## 14. Phase 2 implementation status (stdio: console I/O via #48/#49) — SELESAI

> Status: diimplementasi, di-link dengan **0 undefined symbol**, **dan dijalankan
> di QEMU** (bukan uji link). Tidak ada perubahan `kernel/`, syscall ABI, atau
> app Kyuzen lain. `limine.conf` repo tidak diubah. Sumber LLVM tetap
> `llvmorg-22.1.8` (hanya `entrypoints.txt`/`headers.txt` x86_64 yang ditambah,
> sama seperti Phase 1).

### 14.1 Investigasi baremetal stdio 22.1.8 (temuan, bukan asumsi v24)

Hook vendor persis (`src/__support/OSUtil/baremetal/io.h`):

```c
struct __llvm_libc_stdio_cookie;  // opaque, isi bebas milik vendor
extern "C" struct __llvm_libc_stdio_cookie __llvm_libc_stdin_cookie;
extern "C" struct __llvm_libc_stdio_cookie __llvm_libc_stdout_cookie;
extern "C" struct __llvm_libc_stdio_cookie __llvm_libc_stderr_cookie;
extern "C" ssize_t __llvm_libc_stdio_read(void *cookie, char *buf, size_t size);
extern "C" ssize_t __llvm_libc_stdio_write(void *cookie, const char *buf, size_t size);
```

Semantik return (`src/stdio/baremetal/file_internal.h`): byte yang
ditransfer, atau negatif `-errno` saat gagal (pemanggil mengubahnya menjadi
`libc_errno`). Tidak ada cookie/stream hook lain — tidak ada struct FILE,
tidak ada buffering, tidak ada seek/flush/close. `FILE*` ADALAH alamat cookie
(`OSUtil/baremetal/io.cpp`: `stdin = (FILE*)&__llvm_libc_stdin_cookie`, dst.).

Fungsi yang benar-benar tersedia baremetal (`src/stdio/baremetal/`, 22.1.8):
`printf/vprintf/fprintf/vfprintf`, `puts/fputs/putchar/fputc`,
`getchar/fgetc/fgets`, `fread/fwrite`, `fscanf/scanf/vfscanf/vscanf`,
`feof/ferror` (stub, selalu 0), `remove` (stub, selalu -1). Tidak ada
`fclose/fflush/fseek/ftell/ungetc/fopen/setvbuf` — sesuai audit §2.
Generik (tanpa hook OS, murni `printf_core`/`scanf_core`):
`sprintf/snprintf/vsprintf/vsnprintf/sscanf/vsscanf` (+`asprintf/vasprintf`
yang butuh malloc). Buffering: tidak ada — `fwrite`/`fputc` langsung satu hook
call; `printf` memakai `FlushingBuffer` 1024 B di stack lalu flush per
panggilan (`baremetal/vfprintf_internal.h`). Config baremetal mematikan float
printf (`config/baremetal/config.json`: `PRINTF_DISABLE_FLOAT=true`, dst.),
jadi tidak ada syarat FP/builtins. Tidak ada dependency tree besar: stdio
hanya menarik `printf_core` + `OSUtil` + `errno` (yang semuanya sudah ada).

### 14.2 Hook → syscall mapping (port layer)

| Hook libc | Syscall KyuzenOS | Cookie → fd |
|---|---|---|
| `__llvm_libc_stdio_write` | `int 0x80` #49 (`RAX=49, RBX=fd, RCX=buf, RDX=size` → byte/`-1`) | `&stdout_cookie`→1, `&stderr_cookie`→2, lainnya→`-EBADF` |
| `__llvm_libc_stdio_read` | `int 0x80` #48 (konvensi sama → byte/`-1`) | `&stdin_cookie`→0, lainnya→`-EBADF` |

Abstraksi syscall tidak digandakan: `#define KZ_SYS_READ/WRITE` + dua
`static inline` mengikuti pola `kyuzen_sys_alloc` Phase 1. `size==0` → `0`
tanpa syscall; `buf==NULL` → `-EBADF`. Tipe struct didefinisikan di dalam
`namespace LIBC_NAMESPACE_DECL` karena `io.h` mendeklarasikannya di sana
(definisi di global scope = tipe berbeda = error compiler); objeknya
`extern "C"` sehingga namanya cocok dengan rujukan `io.cpp.obj`. Port
meng-include `src/__support/OSUtil/io.h` agar compiler memeriksa tanda tangan
hook. `ssize_t` LLVM = `long` di x86_64 LP64 — hook dideklarasikan `long`
(ABI identik, linkage C) supaya port tidak bergantung pada header generated.

### 14.3 Files changed

| File | Jenis | Isi |
|---|---|---|
| `libs/libc-port/src/kyuzen_libc_port.cpp` | dimodifikasi | §14.2: cookie struct + 3 objek + read/write hooks (satu-satunya bagian port yang tahu stdio) |
| `libc/config/baremetal/x86_64/entrypoints.txt` | dimodifikasi | +19 entrypoint stdio (§14.4) |
| `libc/config/baremetal/x86_64/headers.txt` | dimodifikasi | +`libc.include.stdio` (hdrgen kini menghasilkan `stdio.h`) |
| `tools/libc-phase2/libc_phase2.c` | baru | smoke test runtime (§14.5) |
| `tools/libc-phase2/libc_app.ld` | baru | identik `libc_app.ld` Phase 1 (`ENTRY(_start)`, 2 PT_LOAD) |
| `tools/libc-phase2/run-qemu.sh` | baru | automasi QEMU (salinan pola Phase 1: disk segar, sendkey via monitor, watchdog; marker `[phase2]`) |
| `Makefile` | aditif | `libc-phase2`, `libc-phase2-qemu`, `LIBC_PHASE2_*`, copy `libc_phase2.elf` ke ISO bila ada; conf uji di `build/libc/iso2/` (tidak bentrok `build/libc/iso/`) |
| `docs/design/audit-llvm-libc-22-freestanding.md` | modifikasi | bagian §14 ini |

### 14.4 Stdio APIs enabled (19) vs unsupported

Aktif: `printf vprintf fprintf vfprintf snprintf vsnprintf sprintf vsprintf`
`puts fputs putchar fputc getchar fgetc fgets fread fwrite feof ferror`.

Sengaja dikecualikan (bukan keterbatasan hook, melainkan keputusan subset):

- `putc/getc` — tidak ada target `baremetal.putc/getc` yang lengkap di 22.1.8
  (hanya implementasi generik File-based yang butuh dukungan OS file);
  pakai `fputc/fgetc/putchar/getchar`.
- `scanf/fscanf/vscanf/vfscanf/sscanf/vsscanf` — terpetakan bersih ke hook
  yang sama, tetapi butuh input stdin deterministik yang tidak diuji (§14.6);
  ditunda agar subset tetap minimal.
- `asprintf/vasprintf` — butuh malloc (bisa, Phase 1 ada) tetapi di luar
  daftar yang diminta; ditunda.
- `remove` (stub selalu -1), `fclose/fflush/fseek/ftell/ungetc/fopen/setvbuf`
  — tidak ada backend-nya di baremetal 22.1.8; mengaktifkannya berarti FILE*
  fiktif. Tidak disentuh.
- `%f` float — dimatikan config baremetal (`PRINTF_DISABLE_FLOAT`); smoke
  test tidak memakai `%f`.

### 14.5 Hasil smoke test QEMU (bukti serial COM1)

```
root@kyuzen> [phase2] start
[phase2] num=-12345 str=kyuzen hex=ff ch=Q u=4294967295
[phase2] printf ok
[phase2] puts ok
Z
[phase2] snprintf ok
[phase2] sprintf ok
[phase2] fprintf stdout ok
[phase2] fprintf stderr ok
[phase2] fwrite ok
[phase2] malloc+stdio m=1234
[phase2] PASS
```

Baris `Z` tunggal = bukti `putchar('Z')` + `putchar('\n')` (tidak mengandung
marker sehingga tidak muncul di ringkasan `grep [phase2]` Makefile, tetapi ada
di `phase2-serial.log` baris 145). Setiap `ok` didahului assertion runtime:
return `printf`/`fprintf` dibandingkan `strlen` string ekspektasi persis;
buffer `snprintf`/`sprintf` dibandingkan `strcmp` byte-per-byte SEBELUM
mencetak `ok`; `fwrite` return = jumlah item; `FAIL` memakai syscall tulis
mentah #49 (terlihat walau stdio rusak) + `return 42`.

### 14.6 Build & artifact

```
export PATH="/e/Tools/msys2/usr/bin:/e/Tools/msys2/clang64/bin:$PATH"
make libc-phase0 CMAKE="..." LIBC_PYTHON_EXE=...   # rebuild archive (entrypoints berubah)
make libc-phase1          # port + app Phase 1 (regresi link, 0 undefined)
make libc-phase1-qemu     # regresi runtime Phase 1: PASS
make libc-phase2          # port + app Phase 2 (0 undefined)
make libc-phase2-qemu     # runtime Phase 2: [phase2] PASS
make && make apps && make boot_image.iso   # regresi default
llvm-nm --undefined-only <phase2 ELF>      # kosong
```

| Artifact | Ukuran | Catatan |
|---|---|---|
| `build/libc/x86_64-pc-none-elf/lib/libc.a` | 500.4 KiB, 197 member | +19 member vs Phase 1 (tepat 19 entrypoint §14.4 + `stdio.h` generated 2.1 KiB) |
| `build/libc/x86_64-pc-none-elf/bin/libc_phase2.elf` | 30.7 KiB | ELF64 EXEC, entry `_start`, 2 PT_LOAD, **0 undefined symbol** |
| `build/libc/port/kyuzen_libc_port.o` | 2.6 KiB | +cookie/read/write (Phase 1: 2.072 B) |
| `build/boot_image.iso` (default) | 12.5 MiB | conf default: **0** kemunculan `libc_phase` |

### 14.7 Known limitations Phase 2

1. **stdin terpetakan tetapi tidak diuji runtime.** Hook read → syscall #48
   fd 0 ada dan entrypoint `getchar/fgetc/fgets/fread` ikut di archive, tetapi
   smoke test tidak memanggilnya: harness QEMU tidak bisa memberi input
   deterministik ke fd 0 tanpa berebut dengan shell yang menjalankan app
   (STOP condition #4 → subset stdout/write yang diklaim lolos; sisi read
   berstatus "termapping, belum terbukti").
2. **`libc_errno` pasca-stdio-gagal = 1, bukan kode presisi.** Kernel
   mengembalikan `-1` polos (bukan `-errno`), sementara `file_internal.h`
   menghitung `errno = -ret`. Deteksi gagal (EOF/return pendek) tetap benar.
   Tidak bisa diperbaiki tanpa ubah kernel — di luar scope.
3. **`feof/ferror` selalu 0** (stub upstream baremetal, bukan bug port).
4. **`%f`/`%e` mencetak tanpa bagian float** (float printf dimatikan config
   baremetal 22.1.8) — jangan pakai format float sampai Phase libm.
5. **Tidak ada buffering antar-panggilan**: tiap `printf` = ≥1 syscall #49
   ( guidance: gabungkan format, jangan `putchar` per char di loop panas).
6. **Incremental cmake tidak mendeteksi perubahan `entrypoints.txt`.**
   Setelah mengedit `entrypoints.txt`/`headers.txt`, `make libc-phase0`
   me-reconfigure tetapi archive tetap basi (178 member, tanpa stdio) —
   harus `rm -rf build/libc/cmake` + configure ulang dari nol (saat itu 197
   member).']"). Guideline: tiap ubah entrypoints, hapus `build/libc/cmake`
   dulu. `libc/docs/configure.rst` yang diregenerasi cmake dikembalikan
   (`git checkout`, sama seperti §12.7.4).
7. QEMU harus di PATH (`E:\Tools\msys2\qemu` tidak otomatis di PATH):
   `make libc-phase*-qemu` gagal `command not found` tanpa itu — murni
   lingkungan, bukan regresi (ditemukan saat validasi Phase 2, Phase 1 lolos
   setelah PATH diperbaiki).
8. Batasan Phase 1 (§13.7) tetap berlaku (arena 1 MiB, errno tanpa TLS,
   tanpa `.init_array`).

### 14.8 Batas eksplisit tahap ini

Belum dan sengaja belum dikerjakan: scanf runtime test, `asprintf`,
`putc/getc`, time (`__llvm_libc_timespec_get_*`), fenv/libm, threads/TLS,
dynamic linking, C++/libc++abi/libunwind, integrasi ke `user_apps/Makefile`
global, dan perubahan apa pun di `kernel/`.

---

## 15. Phase 3 — Kyuzen C SDK (boundary, bukan ekspansi libc)

Phase 3 tidak menambah entrypoint/hook apa pun — ia mengemas hasil Phase 0–2
menjadi SDK yang dapat dipakai developer tanpa mengenal internal LLVM.
Dokumen boundary: `docs/design/kyuzen-c-sdk.md`; sumber boundary yang
di-commit: `sdk/c/linker/app.ld` + `sdk/c/README.md`; staging generated
(`build/sdk/c/`: `include/` + `lib/libc.a` + `crt/crt.o` + `linker/app.ld`)
lewat `make sdk-c` (dengan guard anti-stale dan anti-`third_party`);
app uji `tools/libc-phase3/sdk_smoke.c` lolos QEMU (`[phase3] PASS`, ELF
35.2 KiB, 0 undefined). Detail struktur, flag kanonis, dan limitasi ada di
dokumen SDK — tidak diduplikasi di sini agar daftar limitasi punya satu
sumber kebenaran.
