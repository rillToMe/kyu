# Testing

KyuzenOS has three tiers of tests: host-side unit tests (no QEMU), QEMU probes
(automated GUI/harness tests), and in-OS test programs (Ring-3 ELFs run inside
the system).

## Host-Side Unit Tests

These build and run on the host with `clang`/`clang++`, without QEMU. Many link
the **real** production sources against stubbed syscalls or mock hardware, so a
bug in the kernel or library code shows up here rather than only in QEMU.

Host tests are a **separate CMake invocation** with its own build tree
(`build/host/`). They must not inherit the bare-metal toolchain — that would
target `x86_64-pc-none-elf` and break their link against the host libc.

```sh
./build.sh test          # configure + build + run every test
```

Or drive them directly:

```sh
cmake -S tests/host -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

A single test:

```sh
ctest --test-dir build/host -R test-color --output-on-failure
```

### Registered tests

20 tests are registered; 17 run and pass. The three marked *disabled* are
explained under [Disabled tests](#disabled-tests).

| Test | Coverage |
| --- | --- |
| `test-color` | Color types, blending, color space, UI utilities |
| `test-color-cxx-headers` | The colour headers must also be valid C++ (`-fsyntax-only`) |
| `test-text` | Font rasterization pipeline with a mock backend: UTF-8, OOM injection, cache, measure/render, damage bbox, missing glyph |
| `test-text-ft` | FreeType archive build + raster of a real glyph |
| `test-media` | Media abstraction: type table, path utils, scan, PNG/BMP header probe, fit/scale, LRU cache |
| `test-lexbor-host` | Lexbor HTML/DOM parsing on the host |
| `test-libc-heap` | C SDK heap allocator — **disabled, does not compile** |
| `test-kyuzenfs-tree` | Real `kernel/fs/*` on a RAM-mocked ATA: path resolve, tree ops, rename, readdir, errors — **disabled, fails at run time** |
| `test-libui-theme` | Widget toolkit theme + render pixel checks |
| `test-libui-xml` | XML declarative inflater |
| `test-libui-fileman_widgets` | File manager widget contract |
| `test-libui-owner` | Widget ownership propagation |
| `test-desktop` | Desktop manifest discovery + crash-notice lifecycle |
| `test-netsock` | Real socket layer vs a fake lwIP: close/reuse, exit cleanup, cross-process denial, stale PCB, kill-while-waiting, RX flood, timeouts, exec re-own, TCP client |
| `test-netdns` | Real DNS resolver vs a fake `dns.h`: cached/async/negative/timeout, validation |
| `test-netutil` | `libs/core/netutil.c`: IPv4 parse, resolve fast-path, dial cleanup |
| `test-browser-url-http` | URL parsing and HTTP framing |
| `test-browser-html` | HTML parsing and DOM recovery |
| `test-browser-css-layout` | CSS cascade + block/inline layout + hit-test |
| `test-tls` | BearSSL with prebuilt trust anchors, driven through the Python harness |
| `test-tls-bin` | The TLS binary itself — **disabled**; `test-tls` runs it |

### Disabled tests

Three tests are registered but do not run. All three fail in the Make build too
— they are source-level issues, recorded here so the baseline stays explicit
rather than being quietly deleted.

| Test | Why | Notes |
| --- | --- | --- |
| `test-tls-bin` | Not meant to run standalone | `test-tls` invokes the same binary through the Python harness |
| `test-kyuzenfs-tree` | Exits `0xC0000016` (`STATUS_ILLEGAL_INSTRUCTION`) with no output | **Builds cleanly** — only the run fails |
| `test-libc-heap` | Fails to **compile**: LLVM libc API drift (`write_to_stderr` no longer exists) | Also excluded from the default build |

`test-libc-heap` is excluded from the default build as well as disabled. CTest's
`DISABLED` property only skips a test at *run* time — Ninja still built the
target as part of `all`, the compile failed, and the whole build stopped before
ten unrelated test binaries linked. It can still be built on demand to watch
the error:

```sh
cmake --build build/host --target test-libc-heap
```

`test-kyuzenfs-tree` deliberately stays in the default build: it compiles
cleanly, so building it is real coverage even while the test is disabled.

### Dead targets

Some tests that the Make build declared no longer have sources in the
repository. They are not registered, and configure prints a warning naming each
one so the gap is visible rather than silent:

```text
test-virtqueue   test-virtio-cmd   test-cred        test-proc
test-kill        test-fd           test-pipe        test-fork
test-kyuzenfs-v4 test-kyuzenfs-xcheck test-panic    test-ata
test-textedit
```

## QEMU Probes

Python harnesses in `tests/host/probes/` boot the built ISO headless, drive it
with QEMU `sendkey`/monitor commands, take screen dumps, and parse the serial
log.

These are invoked ad-hoc rather than through named build targets. See
`tests/host/probes/` for the available harnesses and their arguments.

## In-OS Test Programs

Ring-3 ELFs built from `tests/target/` and installed via `limine.conf`. Run them
from the shell:

| Program | Coverage |
| --- | --- |
| `badptr` | 27-assertion pointer-safety suite (Ring-3 boundary) |
| `exit_test` | Exit codes and `waitpid` |
| `fd_test` | fd open/dup/shared-offset/dup2/invalid |
| `pipe_test` | Pipe create/direction/write-read/dup/EOF/broken-reader + SMP roundtrip |
| `fork_test` | Fork return values, PID/PPID, creds, memory independence, fd sharing, pipe inheritance, blocked-child kill |
| `kill_test` | Kill policy matrix, cross-CPU kill, status 125, CPU-bound kill observation |
| `procinfo` | PID/PPID/UID/GID and argv |

Example: `start fork_test` (prints PASS on success).

## Debug-Flavored Boots

The Make build had `conc`, `stress`, `heap-stress` and `heap-watch` targets that
rebuilt the kernel with extra sources and flags. **These do not exist in the
CMake build and were already broken in the Make build**: they appended
`tests/host/unit` or a `debug/` directory that is not in the repository to the
kernel source list, which would attempt to freestanding-compile host test files.

To reproduce one today, pass the extra sources and defines yourself:

```sh
cmake -S . -B build/stress -G Ninja \
      -DCMAKE_C_FLAGS="-DSTRESS_TEST"
```

## Continuous Integration

There is **no CI** configured in the repository. All testing is local and
manual.

## Related Documentation

- [Building](building.md)
- [Running](running.md)
- [Debugging](debugging.md)
