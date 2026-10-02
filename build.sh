#!/usr/bin/env bash
# ============================================================================
# build.sh — run the KyuzenOS CMake build from any MSYS2 shell.
#
#   ./build.sh              # kernel + libs + apps
#   ./build.sh iso          # + Rust + ISO staging + boot_image.iso
#   ./build.sh run          # boot in QEMU
#   ./build.sh test         # host tests (separate build tree)
#   ./build.sh clean        # remove every build directory
#   ./build.sh setup        # only fix PATH, print what was found
#   ./build.sh <target>     # any other ninja target (kyuzen-kernel, kyuzen-calc, ...)
#
# WHY THIS EXISTS
#   MSYS2 ships two complete toolchains, and this project needs tools from BOTH
#   of them — in a combination no single PATH ordering can produce:
#
#     clang, nasm, xorriso, ld.lld   must come from  usr/bin    (LLVM 21)
#     cmake, ninja                   must come from  clang64/bin (native Windows)
#     qemu-system-x86_64             must come from  mingw64/bin
#
#   The trap is cmake. /usr/bin/cmake is a Cygwin binary: it writes POSIX paths
#   (/e/Project/...) into the Ninja files it generates, and native Ninja cannot
#   resolve them. A build configured with it fails with
#
#       ninja: error: '.../CMakeScratch/TryCompile-.../testCCompiler.c',
#       needed by '...testCCompiler.c.obj', missing and no known rule to make it
#
#   which looks like a broken source tree rather than a wrong cmake.
#
#   The trap on the other side is clang. If clang64/bin comes first on PATH,
#   `clang` resolves to LLVM 22 instead of 21. That builds successfully and
#   produces a BYTE-DIFFERENT kernel, silently breaking parity with the
#   verified baseline (96b79a92...e827cb).
#
#   So: usr/bin first for clang, but cmake invoked by absolute path from
#   clang64/bin. That is what this script does.
#
#   Run it from a plain CLANG64/UCRT64/MSYS shell — it does not care which.
# ============================================================================
set -euo pipefail

# ---------------------------------------------------------------------------
# Locate the MSYS2 root from this script's own path, so the script works no
# matter where the tree is checked out or which drive it lives on.
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Find the MSYS2 root by walking up until usr/bin/cmake.exe appears.
_msys_root=""
_d="$SCRIPT_DIR"
while [ "$_d" != "/" ] && [ -n "$_d" ]; do
    if [ -x "$_d/usr/bin/clang.exe" ] && [ -x "$_d/clang64/bin/cmake.exe" ]; then
        _msys_root="$_d"
        break
    fi
    _d="$(dirname "$_d")"
done

# Not inside MSYS2 (e.g. the tree lives on another drive): fall back to the
# conventional install location, then to whatever is already on PATH.
if [ -z "$_msys_root" ]; then
    for _cand in /e/Tools/msys2 /c/msys64 /c/tools/msys64; do
        if [ -x "$_cand/usr/bin/clang.exe" ] && [ -x "$_cand/clang64/bin/cmake.exe" ]; then
            _msys_root="$_cand"
            break
        fi
    done
fi

if [ -z "$_msys_root" ]; then
    echo "build.sh: could not locate an MSYS2 installation." >&2
    echo "  Looked for a directory containing usr/bin/clang.exe and clang64/bin/cmake.exe," >&2
    echo "  starting from $SCRIPT_DIR and falling back to /e/Tools/msys2, /c/msys64." >&2
    echo "  Set KYUZEN_MSYS_ROOT to override." >&2
    exit 1
fi

if [ -n "${KYUZEN_MSYS_ROOT:-}" ]; then
    _msys_root="$KYUZEN_MSYS_ROOT"
fi

# ---------------------------------------------------------------------------
# PATH: usr/bin FIRST (clang 21 wins), then the native-Windows toolchains.
#
# The cargo directory is added because rustup installs outside MSYS2. Note that
# $HOME and $USERPROFILE differ under MSYS2, and cargo lives under the latter.
# ---------------------------------------------------------------------------
_cargo_dir=""
if command -v cygpath >/dev/null 2>&1 && [ -n "${USERPROFILE:-}" ]; then
    _cargo_dir="$(cygpath -u "$USERPROFILE")/.cargo/bin"
fi

export PATH="$_msys_root/usr/bin:$_msys_root/clang64/bin:$_msys_root/mingw64/bin:${_cargo_dir:+$_cargo_dir:}$PATH"

# cmake by ABSOLUTE PATH from clang64: /usr/bin/cmake generates POSIX paths that
# native Ninja cannot read (see the header comment).
CMAKE="$_msys_root/clang64/bin/cmake.exe"

# ctest for the same reason, and it fails in a different way. /usr/bin/ctest is
# a Cygwin binary that mixes path styles when it invokes a test: it produces
#     build/host/E:/Project/.../build/host/bin/test-color.exe
# — a POSIX prefix glued to an absolute Windows path. Every test then reports
# BAD_COMMAND ("Process not started ... no such file or directory"), which looks
# like a broken test suite rather than a wrong ctest.
CTEST="$_msys_root/clang64/bin/ctest.exe"

BUILD_DIR="build"
TARGET_DIR="build/target"
HOST_BUILD_DIR="build/host"
GENERATOR="Ninja"

# ---------------------------------------------------------------------------
# Sanity check: report the toolchain, and make the clang version loud.
#
# Getting this wrong is not obvious at build time — a Clang 22 kernel compiles
# and links fine, it is just not the baseline binary.
# ---------------------------------------------------------------------------
report_toolchain() {
    local _clang_ver
    _clang_ver="$(clang --version 2>/dev/null | head -1 || echo 'clang: MISSING')"

    echo "KyuzenOS build"
    echo "  MSYS2 root : $_msys_root"
    echo "  cmake      : $CMAKE"
    echo "  clang      : $_clang_ver"

    case "$_clang_ver" in
        *"version 21."*) : ;;
        *MISSING*) echo "  ERROR: clang not found. Is $_msys_root/usr/bin on PATH?" >&2; exit 1 ;;
        *) echo ""
           echo "  WARNING: expected Clang 21, got: $_clang_ver" >&2
           echo "  The kernel will build, but it will NOT match the verified baseline" >&2
           echo "  (sha256 96b79a92...e827cb). Check that $_msys_root/usr/bin is first" >&2
           echo "  on PATH, ahead of any clang64/bin entry." >&2
           echo "" ;;
    esac

    for _t in nasm xorriso ld.lld ninja; do
        if ! command -v "$_t" >/dev/null 2>&1; then
            echo "  WARNING: $_t not found on PATH" >&2
        fi
    done
    if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
        echo "  note: qemu-system-x86_64 not found — the run targets will not work" >&2
    fi
    if ! command -v cargo >/dev/null 2>&1; then
        echo "  note: cargo not found — Rust apps will be skipped and 'iso' will fail" >&2
    fi
}

# ---------------------------------------------------------------------------
# Configure if needed. Reconfiguring is cheap (a few seconds) and keeps the
# build tree in sync with any CMakeLists.txt edit, so it always runs.
# ---------------------------------------------------------------------------
configure() {
    "$CMAKE" -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/$TARGET_DIR" -G "$GENERATOR"
}

# ---------------------------------------------------------------------------
# fetch_llvm — clone llvm-project sparsely at the pinned tag, then copy the
# Kyuzen-specific libc config into place.
#
# WHY SPARSE
#   Full llvm-project is ~1.9 GB. This project uses only libc and libcxx (plus
#   the cmake modules and runtimes wrapper they depend on) — about 90 MB of
#   working tree. The rest is clang, llvm, mlir, lldb, flang, polly... none of
#   which is compiled or even read here.
#
# WHY --filter=blob:none --no-checkout FIRST
#   It fetches the commit graph and trees but no file contents, so the sparse
#   filter applies before anything large is downloaded. A plain `git clone`
#   followed by `sparse-checkout set` would still download the full pack.
#
# WHY THE CONFIG FILES ARE COPIED
#   libc/config/baremetal/x86_64/ and the x86_64-pc-none-elf cache are OURS;
#   upstream ships no x86_64 baremetal variant. They used to live inside the
#   (gitignored) clone, tracked nowhere — a re-clone would have destroyed them.
#   They now live in third_party/stdlib/kyuzen/ and are copied in here.
# ---------------------------------------------------------------------------
LLVM_REPO="https://github.com/llvm/llvm-project.git"
LLVM_TAG="llvmorg-22.1.8"
# The commit the project's parity baseline was built with. Same as LLVM_TAG —
# the tag is annotated, so `git rev-parse llvmorg-22.1.8^{commit}` resolves to
# this. Kept explicit so the checkout cannot silently follow a moved tag.
LLVM_COMMIT="ca7933e47d3a3451d81e72ac174dcb5aa28b59d1"
# Overridable so the fetch path can be exercised against a scratch directory.
LLVM_DIR="${KYUZEN_LLVM_DIR:-$SCRIPT_DIR/third_party/stdlib/llvm-project}"
KYUZEN_LLVM_CFG="$SCRIPT_DIR/third_party/stdlib/kyuzen"
# Only what the build actually reads.
#
#   libc, libcxx      the two libraries this project compiles
#   libcxxabi,
#   libunwind         referenced by libcxx's CMakeLists
#   cmake             Modules/LLVMVersion.cmake, needed by runtimes/
#   runtimes          the nested build's entry point
#   llvm/cmake        GetHostTriple.cmake and friends, included by runtimes/
#                     (CONE MODE LIMITATION: sparse-checkout --cone only matches
#                     whole directories, so `llvm` here would pull the entire
#                     ~1.2 GB llvm/ tree. The nested path is written explicitly
#                     to fetch only the 0.49 MB of CMake modules that is used.)
LLVM_PATHS="libc libcxx libcxxabi libunwind cmake runtimes llvm/cmake"

install_kyuzen_libc_config() {
    # Copy the tracked Kyuzen libc config into the clone, at the paths LLVM's
    # build expects. Idempotent: safe to run on every setup.
    #
    # Only entrypoints.txt and headers.txt are copied. The cache file
    # (x86_64-pc-none-elf.cmake) is NOT: it is passed to the nested configure
    # with -C straight from third_party/stdlib/kyuzen/, and it locates
    # upstream's baremetal_common.cmake relative to its own location. Copying it
    # would put a second copy in the tree that could drift.
    local _src="$KYUZEN_LLVM_CFG/libc"
    local _dst="$LLVM_DIR/libc"
    if [ ! -d "$_src" ]; then
        echo "  WARNING: $_src is missing — the Kyuzen libc config is not in the tree." >&2
        echo "  The C/C++ SDK will fail to build." >&2
        return 1
    fi
    mkdir -p "$_dst/config/baremetal/x86_64"
    cp -f "$_src/config/baremetal/x86_64/entrypoints.txt" "$_dst/config/baremetal/x86_64/"
    cp -f "$_src/config/baremetal/x86_64/headers.txt"     "$_dst/config/baremetal/x86_64/"
    echo "  Kyuzen libc config installed into the clone."
}

fetch_llvm() {
    if [ -f "$LLVM_DIR/runtimes/CMakeLists.txt" ]; then
        echo "llvm-project: already present."
        install_kyuzen_libc_config
        return 0
    fi

    if [ -e "$LLVM_DIR" ] && [ ! -d "$LLVM_DIR/.git" ]; then
        echo "llvm-project: $LLVM_DIR exists but is not a git checkout." >&2
        echo "  Move it aside, then re-run ./build.sh setup" >&2
        return 1
    fi

    # A directory that IS a git checkout but has no runtimes/ is a partial or
    # broken clone (a failed sparse checkout, or a leftover from a previous
    # run). Move it aside rather than deleting it: it is a 137 MB download, and
    # deleting a directory the user may still want is not this script's call.
    if [ -d "$LLVM_DIR/.git" ]; then
        local _aside="${LLVM_DIR}.incomplete.$$"
        echo "llvm-project: existing checkout is incomplete (no runtimes/CMakeLists.txt)."
        echo "  Moving it to ${_aside##*/} and fetching a fresh one."
        echo "  Delete that directory once the build works."
        mv "$LLVM_DIR" "$_aside" || {
            echo "llvm-project: could not move the incomplete checkout aside." >&2
            return 1
        }
    fi

    echo "llvm-project: not present. Fetching sparsely (this takes a few minutes)."
    echo "  repo    : $LLVM_REPO"
    echo "  tag     : $LLVM_TAG  ($LLVM_COMMIT)"
    echo "  paths   : $LLVM_PATHS"
    echo "  ~137 MB working tree + .git instead of ~1.9 GB for a full clone."
    echo ""

    mkdir -p "$(dirname "$LLVM_DIR")"

    # 1. Trees only — no file contents yet, so the sparse filter saves the
    #    bulk of the download.
    if ! git clone --filter=blob:none --no-checkout --depth 1 \
                   --branch "$LLVM_TAG" "$LLVM_REPO" "$LLVM_DIR"; then
        echo "llvm-project: clone failed. Check network access to GitHub." >&2
        return 1
    fi

    # 2. Restrict the checkout, then materialise it.
    git -C "$LLVM_DIR" sparse-checkout init --cone
    git -C "$LLVM_DIR" sparse-checkout set $LLVM_PATHS
    git -C "$LLVM_DIR" checkout "$LLVM_TAG"

    # 3. Pin and verify. A moved or mis-resolved tag would silently change the
    #    libc the whole SDK is built from.
    local _head
    _head="$(git -C "$LLVM_DIR" rev-parse HEAD)"
    if [ "$_head" != "$LLVM_COMMIT" ]; then
        echo "llvm-project: WRONG COMMIT after checkout." >&2
        echo "  expected $_LLVM_COMMIT" >&2
        echo "  got      $_head" >&2
        return 1
    fi
    echo "  checked out $LLVM_TAG at ${_head%"${_head#????????}"}..."

    install_kyuzen_libc_config
    echo "llvm-project: done."
}

case "${1:-all}" in
    setup)
        report_toolchain
        echo ""
        fetch_llvm
        echo ""
        echo "To build, run: ./build.sh"
        ;;

    fetch-llvm)
        # Same work as `setup` minus the toolchain report, for scripted use.
        fetch_llvm
        ;;

    clean)
        echo "Removing build directories..."
        rm -rf "$SCRIPT_DIR/$BUILD_DIR"
        echo "  done. Run ./build.sh to rebuild from scratch."
        ;;

    test)
        report_toolchain
        echo ""
        # Host tests are a SEPARATE CMake invocation: they must not inherit the
        # bare-metal toolchain. See tests/host/CMakeLists.txt.
        "$CMAKE" -S "$SCRIPT_DIR/tests/host" -B "$SCRIPT_DIR/$HOST_BUILD_DIR" -G "$GENERATOR"
        "$CMAKE" --build "$SCRIPT_DIR/$HOST_BUILD_DIR"
        "$CTEST" --test-dir "$SCRIPT_DIR/$HOST_BUILD_DIR" --output-on-failure
        ;;

    mkfs)
        report_toolchain
        echo ""
        # The KyuzenFS disk formatter is a HOST program, so it lives in the host
        # build tree, not the target one. `./build.sh mkfs.kyuzenfs` cannot work
        # — that would be looked up as a target build target and fail with
        # "unknown target". This subcommand builds it from the right tree and
        # prints how to use it.
        "$CMAKE" -S "$SCRIPT_DIR/tests/host" -B "$SCRIPT_DIR/$HOST_BUILD_DIR" -G "$GENERATOR"
        "$CMAKE" --build "$SCRIPT_DIR/$HOST_BUILD_DIR" --target mkfs.kyuzenfs
        echo ""
        echo "Built: $SCRIPT_DIR/$HOST_BUILD_DIR/mkfs.kyuzenfs.exe"
        echo "Usage: $SCRIPT_DIR/$HOST_BUILD_DIR/mkfs.kyuzenfs.exe disk.img [size_MB]"
        ;;

    all|"")
        report_toolchain
        echo ""
        configure
        "$CMAKE" --build "$SCRIPT_DIR/$TARGET_DIR"
        ;;

    *)
        # Everything else is a Ninja target: iso, run, run-serial, run-wd,
        # kyuzen-kernel, kyuzen-calc, ... Passing an unknown one produces
        # Ninja's own "unknown target, did you mean ..." message.
        report_toolchain
        echo ""
        configure
        "$CMAKE" --build "$SCRIPT_DIR/$TARGET_DIR" --target "$@"
        ;;
esac
