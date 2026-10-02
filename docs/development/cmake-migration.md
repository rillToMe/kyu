# CMake Migration Report

Migration of the KyuzenOS build system from GNU Make to CMake + Ninja, with
Clang/LLD. The governing constraint throughout: **the build system changes, the
output does not.**

| | |
| --- | --- |
| Branch | `feature/64bit-migration` |
| Pre-migration audit | `fab719b` — `docs/development/build-system-audit.md` |
| Migration commits | 12, `1cd2806` … `5f55acd` |
| Parity result | **133 / 133 artifacts BIT-IDENTICAL** |
| Host tests | 17 / 17 pass (matches the Make baseline) |
| Clean build | `rm -rf build && cmake -S . -B build -G Ninja && cmake --build build` |
| Runtime verification | **pending — requires QEMU outside the sandbox** |

---

## 1. Result

### 1.1 Parity

Every artifact the Make build produced is reproduced byte-for-byte.

| Artifact class | Count | Result |
| --- | --- | --- |
| Kernel (`bin/myos.bin`) | 1 | BIT-IDENTICAL — 434,352 B, `sha256:96b79a92…e827cb` |
| Kernel compatibility copies | 2 | BIT-IDENTICAL |
| User-space ELFs | 28 | BIT-IDENTICAL |
| Rust ELFs (`hello-slint`, `control-center`) | 2 | BIT-IDENTICAL |
| ISO staging files | 93 | BIT-IDENTICAL |
| SDK artifacts (`libc.a`, `crt.o`, `app.ld`, `libcxxrt.a`, `cxxrt.o`) | 5 | BIT-IDENTICAL |
| **Total** | **133** | **133 identical, 0 different, 0 missing** |

The kernel hash is the strongest single check: the Make build is byte-identical
across clean rebuilds, so any difference would be a migration defect rather than
acceptable build metadata.

### 1.2 Behaviour

| Check | Make baseline | CMake + Ninja |
| --- | --- | --- |
| Full clean build (all + ISO) | 416 s | **119 s** |
| Kernel incremental (touch `kernel.c`) | 2.7 s | **< 1 s** (2 steps) |
| No-op build | stable | **0 steps**, repeated 3× |
| Host tests | 17 PASS / 2 FAIL | **17 PASS**, 2 DISABLED (same 2) |
| ISO size | 31,617,024 B | **31,617,024 B** |

### 1.3 The three Phase-13 bugs

`rm -rf build && cmake -S . -B build -G Ninja` did **not** work before commit
`f98271b`. Six phases of validation had all passed because every check
reconfigured an *existing* build directory. Three independent failures were
hidden behind that:

1. **The toolchain file was never applied on a fresh configure.**
   `CMAKE_TOOLCHAIN_FILE` is read by `project()`, and `project()` ran before
   anything included `KyuzenToolchain.cmake`. A clean configure therefore
   detected the *host* compiler. It did not fail there — it failed much later
   in an unrelated-looking place (BearSSL's host-triple probe returned empty).
   Existing directories kept working because `CMakeCache.txt` had recorded the
   toolchain path during the very first configure.

2. **`tests/host` did not include `KyuzenGlob.cmake`.**
   It is a separate CMake invocation, so it inherits nothing from the top
   level. After the `CONFIGURE_DEPENDS` removal its `kyuzen_glob()` calls became
   `Unknown CMake command` — but only on a clean configure; the stale
   `build-host` directory had cached its way past it.

3. **`DISABLED` does not keep a test out of the build.**
   CTest's `DISABLED` property only skips a test at *run* time. Ninja still
   built `test-libc-heap` as part of `all`, the compile failed (LLVM libc API
   drift — pre-existing), and Ninja stopped there, so ten unrelated test
   binaries downstream never linked.

The lesson is recorded here because it generalises: **reconfigure-based
validation cannot detect toolchain-wiring defects.** Phase 13 has to be run
with the build directory deleted.

---

## 2. Layout

41 files, 6,366 lines, replacing a 2,503-line Makefile.

```text
CMakeLists.txt                    156   options, subdirs, image, run targets
cmake/
  KyuzenToolchain.cmake           134   bare-metal: x86_64-pc-none-elf, clang, lld
  KyuzenCompilerWrapper.cmake      96   depfile normalisation (shared)
  kyuzen-cc.in                    103   the wrapper template
  KyuzenCompiler.cmake            271   canonical flag sets (INTERFACE targets)
  KyuzenSources.cmake             204   explicit kernel source list
  KyuzenApplication.cmake         450   the three app recipes + link ordering
  KyuzenGlob.cmake                 82   configure-time globbing
  KyuzenLinker.cmake               55   ld.lld link rule
  KyuzenImage.cmake               212   ISO assembly + module guard
  KyuzenQemu.cmake                118   run / run-serial / run-wd
  verify_*.cmake                  353   post-build ELF/archive/SDK guards
  stage_*_headers.cmake           226   header staging
  copy_*.cmake / iso_*.cmake      222   staging helpers
  run_capture.cmake                58   test-harness capture

kernel/CMakeLists.txt             404   16 subsystem OBJECT libraries
apps/CMakeLists.txt               474   3 app recipes, browser order, font blobs
system/CMakeLists.txt              93
libs/…                             736   core, c, cpp, gui, media, text
third_party/…                    1,227   lwip, freetype, lexbor, bearssl
tests/host/CMakeLists.txt         568   20 tests via CTest
rust/CMakeLists.txt                94
```

The pre-migration audit proposed a layout; the realised version differs in
three places, all noted in §7 below.

---

## 3. The hard rules

The migration's hard rules. Each is satisfied:

| Rule | How |
| --- | --- |
| 1. No giant CMakeLists | Root + one per subsystem + `cmake/` modules; largest project file is `tests/host/CMakeLists.txt` at 568 lines (the 723-line `third_party/freetype/CMakeLists.txt` is vendored upstream, untouched) |
| 2. No source rewrite | **Zero** `.c/.cpp/.h` changes. The only source-tree edits are build files. `kernel/kernel.c` is untouched |
| 3. No ABI change | Every ELF bit-identical; symbol sets and section layouts identical |
| 4. No GCC/binutils | clang 21.1.8, ld.lld, nasm, llvm-ar/nm/objcopy only |
| 8/14. No Limine rebuild | Limine binaries are consumed, never built |
| 11. Verify real deps | Object order derived from the audit's §7 graph |
| 12. Targets over globals | `target_*` everywhere; no `include_directories()`, no `link_directories()` |
| 16. Host tests stay host | Separate invocation, no toolchain inheritance |
| 17. Rust stays separate | CMake invokes cargo; it does not try to compile Rust |
| 18. No `GLOB_RECURSE` | Kernel sources explicit; globs are one level and sorted |
| 19. Readable/modular | Named helpers, one target per library/app |

---

## 4. Findings that required code

The migration was not mechanical. Each of these was discovered by a parity
failure and is the reason a specific piece of the build looks the way it does.

### 4.1 Object order is load-bearing (kernel)

`-flavor gnu` must be ld.lld's **first** argument. CMake places
`CMAKE_EXE_LINKER_FLAGS_INIT` *before* target link options, which would put
`-nostdlib` ahead of `-flavor` and make ld.lld reject the line. `-nostdlib` is
therefore passed per-target, exactly as the Makefile's recipes did.

Object order determines symbol layout. `$<TARGET_OBJECTS:…>` is the only form
that preserves position; `target_link_libraries` reorders.

### 4.2 NASM records the source path verbatim

NASM emits an `STT_FILE` symbol containing the path it was handed. The Makefile
ran from the repo root with relative paths, so the kernel contains
`arch/x86/boot.asm`. Custom commands therefore set
`WORKING_DIRECTORY=${KYUZEN_ROOT}` and pass relative source paths.

### 4.3 `add_executable` needs a source

Targets whose content arrives as `$<TARGET_OBJECTS:…>` still need a nominal
source. An empty `.s` produces an empty `.text` with alignment 4, and the linker
pads the combined `.text` with one `int3` byte to satisfy it. The placeholder is
built from an empty `.s` and stripped with
`llvm-objcopy --remove-section=.text`.

### 4.4 App objects precede libraries

A `.a` only resolves symbols referenced by things already on the command line,
so archive position changes which members are pulled in. On Windows the ordered
link line exceeds the 8191-character command limit (~8 KB for `desktop.elf`),
so archives are declared by path (`$<TARGET_FILE:…>`) inside a **response file**
rather than as library targets. `kyuzen_expand_objects()` and
`kyuzen_object_libraries()` exist for this: `INTERFACE` libraries silently drop
their objects, and CMake does not infer a dependency edge from a generator
expression used in a link option.

### 4.5 `-O` ordering: last one wins

`system/*.c`, `apps/viewer.c` and `apps/clock.c` use `CFLAGS_LIB` (-O2), not
`CFLAGS_APP` (-O0). CMake places target options **before** `INTERFACE` options
and clang honours the **last** `-O`, so the interface value would win. A `FLAGS`
parameter was added to `kyuzen_add_plain_app` to pin these.

### 4.6 `$$(ls …)` sorts, and the sort is observable

SDK apps link `color_utils, widget/abi, widget/src` while GUI apps link
`widget/src, widget/abi` — because the Makefile's `$$(ls …)` sorted its
arguments. Only `color_utils.o` is linked, not all three colour objects. The
browser uses two concatenated wildcards (root `*.cpp` then `engine/*.cpp`), and
BearSSL's `int/i31_*.c` splices mid-list.

### 4.7 Font blob symbols come from a relative path

The `../assets/fonts/X.ttf` paths were relative to `apps/`, and the resulting
symbol names depend on that spelling.

### 4.8 LLVM libc headers derive assertions from `__FILE__`

`-I` paths must be relative for these, and `-include <abs-windows-path>` is
rejected because clang reads `E:` as a drive prefix. `cmake -E copy_directory`
dragged 1,668 files of CMake build metadata into the SDK where 72 headers
belonged, so header staging uses an explicit `*.h`-only script. `clang -M`
leaves a trailing newline on the last dependency, which makes
`file(COPY_FILE)` fail with a bare "Invalid argument", and
`ONLY_IF_DIFFERENT` fails on already-identical destinations — hence the explicit
SHA256 comparison in `copy_if_different.cmake`.

### 4.9 xorriso needs a relative path from the build dir

An absolute Windows path is MSYS-mangled. xorriso runs from
`${CMAKE_BINARY_DIR}` with a relative `iso_root`.

### 4.10 `OUTPUT_NAME` gets a platform prefix

CMake prepends `lib` to static archives, so `OUTPUT_NAME "libdesktop"` produced
`liblibdesktop.a` against Make's `libdesktop.a` (commit `5f55acd`). The name is
cosmetic for the linked ELFs but visible in the build tree.

---

## 5. Incremental correctness

Six incremental checks. All pass.

| # | Check | Result |
| --- | --- | --- |
| 1 | No-op build | **0 steps**, repeated 3× |
| 2 | Touch one kernel source | 2 steps (that TU + relink) |
| 3 | Touch one library source | 2 steps (that TU + dependent app) |
| 4 | Touch one app source | that app only |
| 5 | Touch one test source | **2 steps** (was 645) |
| 6 | Change `KYUZEN_ATA_READ_PATH_DEFAULT` | `ata.c` recompiles, kernel relinks, hash changes |

Three defects had to be fixed to get here.

### 5.1 `CONFIGURE_DEPENDS` rebuilt everything

CMake implements `CONFIGURE_DEPENDS` by writing a `VerifyGlobs.cmake` script and
marking it `FORCE`-dirty, so Ninja considered it out of date on **every**
invocation. Re-running it invalidated every globbed source list and cascaded
into a full rebuild — a no-op `ninja` rebuilt 818 of 819 steps.

Replaced by `cmake/KyuzenGlob.cmake`. The first version of that helper cached
results in `CACHE INTERNAL`, which made the cache win over reality: adding a
file and reconfiguring kept returning the stale list, and only `cmake --fresh`
saw the new file. That is *worse* than the `$(wildcard)` it replaces, so nothing
is cached now.

### 5.2 MSYS clang writes POSIX paths into depfiles

MSYS2 ships two clang builds:

```text
usr/bin (MSYS/cygwin) -> /usr/lib/clang/21/include/stdint.h   POSIX
clang64/bin (MINGW)   -> E:/Tools/msys2/clang64/include/...   native
```

Ninja is a native Windows binary and cannot resolve `/usr/…`, so it treats every
such dependency as MISSING and the target is permanently out of date. The
symptoms are a badly misleading pair: a no-op `ninja` reports hundreds of
rebuilds, `ninja -d explain` blames `stdint.h is dirty`, and `ninja -n` reports
nothing to do at all.

The MINGW clang emits native paths but is LLVM 22 and produces a byte-different
kernel, so it cannot be used without breaking parity. Instead a wrapper
(`cmake/kyuzen-cc.in` → `build-tools/kyuzen-cc`, `kyuzen-cxx`) runs the real
compiler and rewrites the depfile in place. Only `/usr/` is rewritten: MSYS2
exposes `/lib` as a symlink to `usr/lib`, no depfile in this tree contains it,
and mapping it to `<root>/lib` would point at a directory that does not exist.

Two silent bugs had to be fixed in the wrapper itself:

- Ninja passes `-MF` with **literal backslashes** (`libs\gui\…\x.obj.d`). `sh`
  passes that through verbatim, so `[ -f "$DEPFILE" ]` probed for a file
  literally named `libs\gui\…d` and skipped the rewrite entirely. Paths are
  normalised to forward slashes first.
- `s|…|…|` collided with the regex alternation in the same expression
  (`unknown option to 's'`). The delimiter is now `#`.

The wrapper is shared with the host build (`fc09e30`). It could not simply be
reused at first: it lived inside `KyuzenToolchain.cmake`, which `tests/host`
must not include — that file sets `--target=x86_64-pc-none-elf` and
`CMAKE_TRY_COMPILE_TARGET_TYPE`, which would switch the host tests to the
bare-metal ABI. The wrapper is triple-agnostic, so it was factored into
`cmake/KyuzenCompilerWrapper.cmake` and is included by both.

Wiring that up exposed a third bug: `CMAKE_CXX_COMPILER` is a **list** when the
wrapper is in use (`sh;build-tools/kyuzen-cxx;clang++`), and
`add_test(COMMAND …)` turns each element into a separate argv entry. The
`test-color-cxx-headers` test therefore ran `sh -std=c++17 …` and failed with
`-d: invalid option`. It now invokes the real compiler directly; it is a
`-fsyntax-only` check, so there is no depfile to normalise.

### 5.3 SDK custom commands did not declare their outputs

The C and C++ SDK staging commands listed only their sentinel file as `OUTPUT`,
so Ninja had no producer edge for the `.a/.o/.ld` files they actually wrote and
treated consumers as always dirty. All real outputs are now declared.

---

## 6. Verification

### 6.1 Commands

```sh
# target build
rm -rf build-cmake build-tools
cmake -S . -B build-cmake -G Ninja
ninja -C build-cmake                 # kernel + libs + apps   (862 steps)
ninja -C build-cmake iso             # + Rust + ISO staging + ISO

# host tests (separate invocation, host toolchain)
rm -rf build-host
cmake -S tests/host -B build-host -G Ninja
ninja -C build-host
ctest --test-dir build-host

# run (requires QEMU on PATH; not verifiable in the build sandbox)
ninja -C build-cmake run
ninja -C build-cmake run-serial
ninja -C build-cmake run-wd
```

`cmake --build build-cmake` is equivalent to `ninja -C build-cmake`.

### 6.2 Parity checks

The parity checks. Results:

| Check | Tool | Result |
| --- | --- | --- |
| Kernel hash | sha256 | `96b79a92…e827cb` — identical |
| Kernel symbols | `llvm-nm --defined-only` | 1,549 both — identical |
| Kernel sections | `llvm-readelf -S` | identical |
| App ELF hashes | sha256 | 30/30 identical |
| App segments | `llvm-readelf -l` | same counts as Make per ELF |
| Undefined symbols | `llvm-nm --undefined-only` | 0 ELFs affected |
| Static archives | `llvm-ar t` | member sets match |
| ISO staging | sha256 | 93/93 identical |
| ISO size | — | 31,617,024 B both |
| Module guard | custom | all 87 `boot():/` references present (86 `module_path` + `kernel_path`) |

### 6.3 Documented differences

Per §16.5 classification:

**EXPECTED** — ISO container hash differs. xorriso embeds timestamps and a fresh
GPT GUID. Payload is identical (93/93 staging files) and the size matches
exactly. This is inherent to the tool, not to the migration.

**HARMLESS BUILD METADATA** — archive member names keep the `.cpp.obj` /
`.c.obj` extension instead of Make's `.o`. CMake ignores
`CMAKE_*_OUTPUT_EXTENSION` on Windows. Member *sets* match, and the linked ELFs
are unaffected (they are bit-identical).

**HARMLESS** — `brssl.exe` (host tool, same 543,288 B) hashes differently: it is
a hosted Windows binary embedding paths and a PE timestamp.

**HARMLESS** — the Make build staged host artifacts (`liblexbor_host.a`,
`brssl.exe`) inside `build/`; the CMake build separates them into `build-host/`.
Member sets match (158/158 for host Lexbor).

**PRE-EXISTING, NOT A REGRESSION** — `calc.elf` contains 33 x87 instructions
(`fmulp`, `faddp`, `fdivrp`) despite `-msoft-float`, and 8 ELFs have a single
`PT_LOAD` rather than two. Both are **bit-identical to the Make build**, so they
are properties of the sources and libraries, not the build system. The audit's
"exactly two `PT_LOAD`" claim describes the three linker *scripts*, which are
unchanged and still produce matching layouts.

No `REQUIRES INVESTIGATION` or `REGRESSION` items remain open.

### 6.4 Host tests

20 tests are registered; 17 run and pass.

| | |
| --- | --- |
| Passing | 17 / 17 |
| Disabled: `test-tls-bin` | Driven through `test-tls`, which runs the Python harness. Kept out of the default build |
| Disabled: `test-kyuzenfs-tree` | Builds, then exits `0xC0000016` with no output — pre-existing, also fails under Make |
| Disabled: `test-libc-heap` | Fails to **compile** (LLVM libc `write_to_stderr` API drift) — pre-existing, also fails under Make |

`test-libc-heap` is `EXCLUDE_FROM_ALL` as well as `DISABLED`: `DISABLED` alone
only skips the test at run time, so Ninja still built it, the compile failed,
and the whole build stopped before ten unrelated test binaries linked. It can
still be built on demand (`ninja -C build-host test-libc-heap`) to watch the
error.

`test-kyuzenfs-tree` stays in the default build deliberately: it compiles
cleanly and only fails at run time, so building it is real coverage.

13 dead targets (sources absent from the repo) produce configure-time warnings
rather than silent absence.

---

## 7. Deviations from the audit's §14 plan

Three, all deliberate.

**`KyuzenSdk.cmake` was not created.** SDK staging lives in
`libs/c/CMakeLists.txt` and `libs/cpp/CMakeLists.txt`, next to the libraries it
stages. Splitting them would have meant passing a dozen paths between files for
no benefit.

**`KyuzenTests.cmake` was not created.** The test helper is defined in
`tests/host/CMakeLists.txt`, which is the only file that uses it. It is a
separate CMake invocation, so a shared module would be dead weight.

**`KyuzenCompilerWrapper.cmake` was added.** Not in the plan; it exists because
both build trees need depfile normalisation and neither may include the other's
toolchain (§5.2).

Also: `third_party/freetype/kyuzen.cmake` mirrors the existing `kyuzen.mk`
naming, because that directory already contains an upstream `CMakeLists.txt`
(644 lines) that must not be modified or used.

---

## 8. Risks from §15, resolved

| # | Risk | Status |
| --- | --- | --- |
| R1 | Kernel object order | Resolved — `$<TARGET_OBJECTS:>` order preserved, kernel bit-identical |
| R2 | `-DNAME=<header>` quoting | Resolved — CMake passes args directly |
| R3 | `-iquote` vs `-I` | Resolved — emitted once per directory via `SHELL:` prefix |
| R4 | LLVM libc nested CMake | Resolved — driven as an external step, never `add_subdirectory` |
| R5 | `.init_array` per recipe | Resolved — each app keeps its own script, never unified |
| R6 | lwIP per-file flags | Resolved — separate `kyuzen-lwip` target |
| R7 | NASM support | Resolved — `ASM_NASM`, `-f elf64`, `WORKING_DIRECTORY` pinned |
| R8 | Archive link order | Resolved — response files preserve order |
| R9 | Host tests and the target toolchain | Resolved — and *strengthened*: the shared wrapper module is included explicitly while the toolchain file is not |
| R10 | `build/` cleanliness | Resolved — all output under the build dir; nothing written into the source tree |
| R11 | Rust `rust-lld` absolute path | Left as-is per Rule 17; unchanged |
| R12 | Missing `limine.exe` / `*.bin` | Documented; ISO target fails clearly |
| R13 | 13 dead test targets | Resolved — `if(EXISTS)` guard + configure warning |
| R14 | Stale-archive guards | Resolved — ported as POST_BUILD commands |
| R15 | Two `myos.bin` outputs | Resolved — both kept, copy is `cmp`-guarded |

---

## 9. Not verified

**QEMU runtime behaviour.** The build sandbox cannot run QEMU, so the following
are unverified and must be checked by hand:

- boot to login
- `zen` shell
- `logout`
- desktop environment and app launch
- `run-serial` panic capture
- `run-wd` serial-to-file

The QEMU command lines were verified against the Makefile by inspection:
`ninja -C build-cmake -t commands run` emits the same argument list
(`-cpu max -m 1G -boot d -smp 8`, `disk.img` + `boot_image.iso`,
`-nic user,model=e1000`, virtio-vga 1920×1080, `gtk,zoom-to-fit=on`, serial to
`serial.log`, `serial.log` removed first). One deliberate correction was made:
the Makefile sets `FULLSCREEN ?= 1` and documents full-screen as the default, but
`FULLSCREEN_ARG` is never referenced by any recipe, so `make run` always
launched **windowed**. The CMake target defaults to windowed to match the
observed behaviour; `-DKYUZEN_QEMU_FULLSCREEN=ON` now genuinely adds
`-full-screen` (commit `53331ee`).

---

## 10. Makefile status

The Makefile is **still present and untouched**. Both build systems work
side-by-side and produce identical artifacts.

Removing it is a separate decision and is not part of this migration: the
Makefile is the reference the parity claims are measured against, and deleting
it would make those claims unverifiable. See §11 for the recommendation.

---

## 11. Recommendation

1. **Run the QEMU checks in §9** before merging. They are the only remaining
   unverified surface.
2. **Keep the Makefile for one release cycle.** It costs nothing to keep, and it
   is the only way to re-verify parity after a change. Once the CMake build has
   booted and run the desktop without incident, removing it is a one-line
   decision.
3. **Do not run `make clean` and `ninja` against the same directory.** The Make
   build writes to `build/`, the CMake build to `build-cmake/`; the separation is
   deliberate and should be preserved until the Makefile is removed.
