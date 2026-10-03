# Contributing to KyuzenOS

KyuzenOS is a 64-bit higher-half operating system written from scratch in C,
C++, and Rust. It boots on bare metal and in QEMU into a kernel with SMP
scheduling, Ring-3 userspace, a composited window manager, its own filesystem,
and a TCP/IP stack.

Contributions are welcome. This document covers the workflow: setting up,
building, testing, and getting a change merged.

For the short binding rules on commits, branches, and pull requests, read
[`RULES.md`](RULES.md). For the rules on how to write the code itself — style,
architecture, error handling, memory safety, synchronization, documentation —
read [`.rules/`](.rules/RULES.md). Both sets are binding.

If you are changing anything under `libs/gui/widget/`, `include/libui*.h`,
`apps/`, `system/desktop/`, or `ui/xml/`, also read
[`.rules/UI.md`](.rules/UI.md). It is binding too: the design-system rules are
numbered (`UI-n.m`) so a review comment can cite one, and a violation is a bug
even if it compiles and looks fine on screen.

## Contents

- [Before You Start](#before-you-start)
- [Development Setup](#development-setup)
- [Build](#build)
- [Test](#test)
- [Workflow](#workflow)
- [Commit Conventions](#commit-conventions)
- [Pull Requests](#pull-requests)
- [When to Open an Issue First](#when-to-open-an-issue-first)
- [Repository-Specific Requirements](#repository-specific-requirements)
- [Third-Party Code](#third-party-code)
- [Reporting Bugs](#reporting-bugs)

## Before You Start

KyuzenOS is low-level systems code: a bug is a crash, a hang, or silent memory
corruption. Two habits matter more than speed here.

1. **Read the surrounding code before changing it.** This repository has one
   authoritative implementation per subsystem. Duplicating a working system is
   a defect, not a shortcut.
2. **Get a clean build and a boot before you edit.** If the tree does not build
   on your machine, you cannot tell your bug from an existing one.

Start with [Architecture Overview](docs/architecture/overview.md) for how the
system fits together, then [Building](docs/development/building.md).

## Development Setup

Full toolchain setup for Windows (MSYS2) and Linux is in
[Building](docs/development/building.md). The short version:

| Tool | Purpose |
| --- | --- |
| `clang` + `ld.lld` | Compile and link the kernel, apps, and host tests |
| `nasm` | Assemble the ISR stubs and GDT/TSS |
| `cmake` (≥ 3.20) + `ninja` | Build orchestration |
| `xorriso` | Build the hybrid BIOS+UEFI ISO |
| `qemu-system-x86_64` | Run the system |
| `python3` | Used by the nested LLVM libc build |
| `rustup` + `rustc` | Optional; needed for the Rust applications |

On MSYS2, run `./build.sh setup` to confirm the toolchain resolves correctly.

### A note on the compiler version

The kernel is verified against **Clang 21**, and `./build.sh` puts the matching
toolchain first on `PATH` for you. Building with a different Clang major version
succeeds but produces a **byte-different kernel** from the verified baseline,
with no error to warn you. Both `./build.sh` and the CMake configure step warn
when this happens. Fix your `PATH` rather than silencing the warning.

### A note on generated files

Everything generated lives under `build/`, which is gitignored. Nothing is
written into the source tree, so `rm -rf build` is a complete reset. Do not
commit build output, `compile_commands.json`, `serial.log`, or disk images.

## Build

Run all commands from the repository root. Use `./build.sh` — it locates MSYS2
and sets `PATH` correctly regardless of which shell you started in.

```sh
./build.sh setup        # FIRST RUN ONLY: fetch llvm-project (~137 MB)
./build.sh              # kernel + libraries + applications
./build.sh iso          # + Rust apps and the bootable ISO
./build.sh run          # build the ISO and boot it in QEMU
./build.sh clean        # remove build/ entirely
./build.sh <target>     # any Ninja target, e.g. kyuzen-kernel, kyuzen-desktop
```

### `./build.sh setup` — why it is needed

The C and C++ SDKs are built from LLVM's `libc` and `libcxx`. Upstream
`llvm-project` is **~1.9 GB**, so it is not committed. Without it, configure
fails with a message pointing at `third_party/stdlib/llvm-project`.

`setup` clones it sparsely — only the seven directories this project actually
compiles — at the pinned tag:

| | |
| --- | --- |
| Size | **~137 MB** instead of ~1.9 GB |
| Time | about five minutes |
| Pinned to | `llvmorg-22.1.8` (`ca7933e4`), verified after checkout |

It also copies the Kyuzen-specific libc configuration from
`third_party/stdlib/kyuzen/` into the clone. That configuration is tracked in
this repository precisely because upstream ships no x86_64 baremetal variant —
it is ours, and it used to live inside the (gitignored) clone where a re-clone
would have destroyed it.

`setup` is safe to re-run: an existing clone is detected and left alone. It also
reports the resolved MSYS2 root, cmake path and clang version.

If you only want the kernel and do not need the SDKs, skip it entirely:

```sh
cmake -S . -B build/target -G Ninja -DKYUZEN_BUILD_LIBC_SDK=OFF
```

Useful individual targets: `kyuzen-kernel`, `kyuzen-desktop`,
`kyuzen-browser`, `kyuzen-rust-apps`, `kyuzen-sdk-c`, `kyuzen-sdk-cpp`.

To run the system, see [Running](docs/development/running.md). Default
credentials are `root` / `1`.

### On Linux

`./build.sh` is written for MSYS2 and expects `usr/bin`, `clang64/bin`, and
`mingw64/bin` under one root. Those paths do not exist on Linux, so configure
CMake directly instead:

```sh
cmake -S . -B build/target -G Ninja
cmake --build build/target
```

## Test

KyuzenOS has three tiers of tests. See [Testing](docs/development/testing.md)
for the full picture.

### Host-side unit tests

These run on your host with no QEMU and cover a large share of the system — many
link the real production sources against mocks, so a kernel or library bug shows
up here.

```sh
./build.sh test          # configure + build + run every test
```

A single test:

```sh
ctest --test-dir build/host -R test-color --output-on-failure
```

20 tests are registered; 17 run and pass. The three disabled ones are documented
with their reasons in [Testing](docs/development/testing.md#disabled-tests) —
they were already failing before the CMake migration, so they are not your bug.

### In-OS tests

Ring-3 test programs run inside the booted system from the shell:

```text
root@kyuzen> start fork_test
root@kyuzen> start badptr
```

| Program | Coverage | Success output |
| --- | --- | --- |
| `badptr` | 27-assertion pointer-safety suite at the Ring-3 boundary | `=== badptr selesai: N PASS, M FAIL ===` |
| `fd_test` | fd open/dup/shared-offset/dup2/invalid | `FD TEST PASS` |
| `pipe_test` | Pipe create/direction/write-read/dup/EOF/broken-reader, SMP roundtrip | `PIPE TEST PASS` |
| `fork_test` | Fork return values, PID/PPID, creds, memory independence, fd sharing | `FORK TEST PASS` |
| `kill_test` | Kill policy matrix, cross-CPU kill, status 125 | `kill_test: PASS` |
| `exit_test` | Exit codes and `waitpid` | Prints the exit code |
| `procinfo` | PID/PPID/UID/GID and argv | Prints the process identity |

The first five report pass or fail explicitly. `exit_test` and `procinfo` print
their results and are read rather than asserted.

### QEMU probes

Python harnesses in `tests/host/probes/` boot the ISO headless, drive it through
the QEMU monitor, and parse the serial log. These are invoked ad-hoc rather than
through named targets; see the directory for the available harnesses.

### Before you open a pull request

- `./build.sh` succeeds with no new warnings.
- `./build.sh test` passes.
- If you touched kernel or userspace behavior, boot it: `./build.sh run`.
- If you fixed a bug, add regression verification. Do not assert that it works —
  show it.

There is **no CI** in this repository. All verification is local, which means
your own build and test run is the only check a change gets before review.
Please be thorough.

## Workflow

```text
Fork → Branch → Develop → Build → Test → Commit → Push → Pull Request → Review → Merge
```

### 1. Fork and clone

Fork the repository on GitHub, then clone your fork:

```sh
git clone https://github.com/<your-username>/<repo>.git
cd <repo>
git remote add upstream https://github.com/rillToMe/kyu.git
```

### 2. Branch

Branch from the default branch, `feature/64bit-migration`:

```sh
git fetch upstream
git switch -c feature/my-change upstream/feature/64bit-migration
```

Name branches `<type>/<short-description>` using `feature/`, `fix/`, `docs/`, or
`refactor/` — see [`RULES.md`](RULES.md#branches).

### 3. Develop

Keep the change focused. Match the style of the file you are editing, reuse the
utilities that already exist, and do not refactor unrelated code.

### 4. Build

```sh
./build.sh
```

### 5. Test

```sh
./build.sh test
```

### 6. Commit

```sh
git commit -m "fix(mm): stop the VMM racing on the user PML4"
```

Follow [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/) —
the types, scopes, and quality bar are in [`RULES.md`](RULES.md#commits).

### 7. Push

```sh
git push -u origin feature/my-change
```

### 8. Pull request

Open the pull request against `feature/64bit-migration`. The repository's pull
request template will prompt you for a summary, the motivation, the changes, and
what you verified. Fill it in.

### 9. Review

A maintainer reviews the change. Expect questions about correctness, memory
safety, synchronization, and whether an existing system should have been
extended instead of a new one added. Review comments are about the code.

### 10. Merge

A maintainer merges the pull request. Do not merge your own.

## Commit Conventions

Follow [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/):

```sh
git commit -m "fix(mm): stop the VMM racing on the user PML4"
```

The format, the allowed types, the scope conventions, and the quality bar are
all defined in [`RULES.md`](RULES.md#commits). Do not restate them here — that
is the file to check.

## Pull Requests

The requirements — target branch, one logical change, passing build and tests —
are in [`RULES.md`](RULES.md#pull-requests). The following is about making
review work well.

- **State what you verified, and how.** "Builds and passes tests" is a claim.
  Say which test you ran and what you observed.
- **Describe known limitations honestly.** Partial work with a clear boundary is
  easier to review than work presented as finished when it is not.
- **Explain any decision a reviewer would question.** If you chose one approach
  over another, say why. It saves a round trip.
- **Keep unrelated cleanup out.** Reformatting a file you happened to open makes
  the real change hard to find.

During review, push additional commits to the same branch rather than
force-pushing — force-pushing discards the reviewer's line comments.

## When to Open an Issue First

Open an issue before you start, rather than after you have written the code, for:

- **A new subsystem, or a new application.** These are large and architectural.
  Confirm the approach before building it.
- **A change to a public interface** — syscall numbers, the on-disk filesystem
  format, application manifests, or a library API. These have compatibility
  consequences beyond your change.
- **A refactor that moves or renames files.** The repository deliberately keeps
  one authoritative path per subsystem.
- **A new third-party dependency.** See [Third-Party Code](#third-party-code).
- **Anything you are unsure is in scope.** A short issue is cheaper than a
  rejected pull request.

You do not need an issue for a bug fix with a clear cause, a documentation
correction, or a small, self-contained improvement.

## Repository-Specific Requirements

These are the constraints that make KyuzenOS what it is. Breaking one produces
code that builds and then fails at runtime.

- **Languages: C, C++, and Rust only.** Do not introduce another language or
  compiler. The one exception is build-orchestration tooling, which is not
  project code.
- **No SSE or x87.** The kernel never sets `CR4.OSFXSR`, so any SSE instruction
  faults at runtime. Everything is built `-mno-sse -msoft-float` deliberately.
  Every user ELF is scanned for SSE/x87 instructions after linking and the
  build fails if any are found. Do not add code that requires floating point,
  and do not remove those flags. This is why parts of vendored upstream code
  (for example Lexbor's CSS modules) are excluded.
- **No standard library in the kernel.** Kernel and userspace code is
  freestanding (`-ffreestanding -nostdlib`). Userspace gets libc through the C
  SDK, not the host.
- **`-mcmodel=kernel` is mandatory** for kernel code — higher-half symbols
  exceed 4 GB.
- **Do not add a second implementation of an existing subsystem.** Extend what
  is there. Where legacy entry points exist, they are retained deliberately and
  documented as such.
- **Respect the concurrency contracts.** Lock ordering, blocking rules,
  ownership, and buffer limits are documented. Violating them is a bug, and it
  will not show up in a single-CPU test.
- **Handle errors.** Never ignore a return value. Kernel code must fail safely.
- **Do not include lwIP headers outside the four files that already do.**
  `kernel/net/net_dns.c`, `net_init.c`, `net_ping.c`, and `net_socket.c` are the
  only files compiled with `kyuzen-flags-lwip`. Adding a lwIP include elsewhere
  will not build.

Documentation is mandatory here rather than optional follow-up — see
[`.rules/DOCUMENTATION.md`](.rules/DOCUMENTATION.md). Before declaring a change
complete, run through [`.rules/REVIEW_CHECKLIST.md`](.rules/REVIEW_CHECKLIST.md).

## Third-Party Code

Vendored dependencies live in `third_party/` — lwIP, LLVM libc/libc++, FreeType,
BearSSL, and Lexbor. Each is integrated through a curated module list rather
than its upstream build system, because the upstream builds assume a hosted
environment and use floating point. The integration and its rationale are
documented in
[Build System Audit](docs/development/build-system-audit.md) and in
`third_party/CMakeLists.txt`.

Two rules apply:

- **Do not modify vendored upstream sources** to make them build. Exclude the
  module, or add a port file alongside it, the way the existing integrations do.
- **Open an issue before adding a new dependency.** Every dependency is a
  long-term maintenance cost, and it must be made to work without SSE and
  without a hosted C library. Explain what it is for and why the existing
  libraries cannot do the job.

## Reporting Bugs

Open an issue using the bug report template. Include what you did, what you
expected, and what happened, plus the relevant `serial.log` excerpt. For a
crash, include the BSOD details — exception vector, registers, and `CR2` — or
the crash report. See [Debugging](docs/development/debugging.md) for how to
capture that.

For security vulnerabilities, do **not** open a public issue. Follow
[`SECURITY.md`](SECURITY.md).

## Related Documentation

- [`RULES.md`](RULES.md) — commits, branches, pull requests
- [`.rules/`](.rules/RULES.md) — style, architecture, build, documentation rules
- [Building](docs/development/building.md) — toolchain setup and build targets
- [Testing](docs/development/testing.md) — the three test tiers
- [Debugging](docs/development/debugging.md) — serial log, BSOD, crash reports
- [Architecture Overview](docs/architecture/overview.md) — how the system works
