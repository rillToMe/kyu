# Testing

KyuzenOS has three tiers of tests: host-side unit tests (no QEMU), QEMU probes
(automated GUI/harness tests), and in-OS test programs (Ring-3 ELFs run inside
the system).

> **Note on target availability:** several `make test-*` targets reference
> source files that are no longer present in the tree. Those targets fail with
> `No rule to make target`. They are listed under
> [Unavailable targets](#unavailable-targets) below.

## Host-Side Unit Tests

These build and run on the host with `clang`/`clang++`, without QEMU. They link
the real production sources against stubbed syscalls or mock hardware.

| Target | Coverage |
| --- | --- |
| `make test-color` | Color types, blending, color space, UI utilities |
| `make test-text` | Font rasterization pipeline with a mock backend: UTF-8, OOM injection, cache, measure/render, damage bbox, missing glyph |
| `make test-text-ft` | FreeType 2.14.3 archive build + raster of a real glyph |
| `make test-media` | Media abstraction: type table, path utils, scan, PNG/BMP header probe, fit/scale, LRU cache |
| `make test-libc-heap` | C SDK heap allocator |
| `make test-kyuzenfs-tree` | Real `kernel/fs/*` on a RAM-mocked ATA: path resolve, tree ops, rename, readdir, errors |
| `make test-libui-theme` | Widget toolkit theme + render pixel checks |
| `make test-libui-xml` | XML declarative inflater |
| `make test-libui-fileman` | File manager widget contract |
| `make test-libui-owner` | Widget ownership propagation |
| `make test-desktop` | Desktop manifest discovery + crash-notice lifecycle |
| `make test-netsock` | Real socket layer vs a fake lwIP: close/reuse, exit cleanup, cross-process denial, stale PCB, kill-while-waiting, RX flood, timeouts, exec re-own, TCP client |
| `make test-netdns` | Real DNS resolver vs a fake `dns.h`: cached/async/negative/timeout, validation |
| `make test-netutil` | `libs/core/netutil.c`: IPv4 parse, resolve fast-path, dial cleanup |
| `make test-browser-url-http` | URL parsing and HTTP framing |
| `make test-browser-html` | HTML parsing and DOM recovery |
| `make test-browser-css-layout` | CSS cascade + block/inline layout + hit-test |
| `make test-tls` | BearSSL with prebuilt trust anchors |

Run them all:

```sh
make test-netsock test-netdns test-netutil test-color test-text test-text-ft \
     test-media test-kyuzenfs-tree test-libui-theme test-libui-xml \
     test-libui-fileman test-libui-owner test-desktop test-libc-heap \
     test-browser-url-http test-browser-html test-browser-css-layout test-tls
```

Most targets execute `./tests/host/unit/<binary>` directly.

## QEMU Probes

Python harnesses in `tests/host/probes/` boot the built ISO headless, drive it
with QEMU `sendkey`/monitor commands, take screen dumps, and parse the serial
log.

| Target | Description |
| --- | --- |
| `make test-fileman-qemu` | Boot → login → `start fileman` → New Folder + rename + delete; verifies window geometry and serial evidence |

Other probes (appearance, browser, dialog, focus, layout, menu, settings, XML)
exist but are invoked ad-hoc rather than through Make targets.

## In-OS Test Programs

Ring-3 ELFs under `tests/target/`, built by `apps/Makefile` and installed via
`limine.conf`. Run them from the shell:

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

| Target | Purpose |
| --- | --- |
| `make conc` | Concurrency suite (sleep/semaphore/mutex-SMP/condvar/priority/vfs-fd) |
| `make stress` | PMM stress test |
| `make heap-stress` | Heap overflow / canary detection |
| `make heap-watch` | Heap watch debug build (serial logging) |

> These targets add `tests/host/unit` or a `debug` directory to the kernel
> source list. See the note below.

## Unavailable Targets

The following `make` targets reference source files that no longer exist in the
repository. They fail with `No rule to make target`:

```text
test-virtqueue   test-virtio-cmd   test-cred        test-proc
test-kill        test-fd           test-pipe        test-fork
test-kyuzenfs-v4 test-kyuzenfs-xcheck test-panic    test-ata
test-textedit
```

In addition, `make conc`, `make heap-stress`, and `make stress` add
`tests/host/unit` (or a `debug/` directory that does not exist) to the kernel
source list. The stale exclusion list does not filter out the host tests that
currently exist there, so these builds attempt to freestanding-compile host
test files. They should be treated as not working until the source list is
corrected.

## Continuous Integration

There is **no CI** configured in the repository. All testing is local and
manual.

## Related Documentation

- [Building](building.md)
- [Running](running.md)
- [Debugging](debugging.md)
