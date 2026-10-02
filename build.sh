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
#     build-host/E:/Project/.../build-host/bin/test-color.exe
# — a POSIX prefix glued to an absolute Windows path. Every test then reports
# BAD_COMMAND ("Process not started ... no such file or directory"), which looks
# like a broken test suite rather than a wrong ctest.
CTEST="$_msys_root/clang64/bin/ctest.exe"

BUILD_DIR="build-cmake"
HOST_BUILD_DIR="build-host"
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
    "$CMAKE" -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/$BUILD_DIR" -G "$GENERATOR"
}

case "${1:-all}" in
    setup)
        report_toolchain
        echo ""
        echo "PATH exported for this shell only. To build, run: ./build.sh"
        ;;

    clean)
        echo "Removing build directories..."
        rm -rf "$SCRIPT_DIR/$BUILD_DIR" "$SCRIPT_DIR/$HOST_BUILD_DIR" "$SCRIPT_DIR/build-tools"
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

    all|"")
        report_toolchain
        echo ""
        configure
        "$CMAKE" --build "$SCRIPT_DIR/$BUILD_DIR"
        ;;

    *)
        # Everything else is a Ninja target: iso, run, run-serial, run-wd,
        # kyuzen-kernel, kyuzen-calc, ... Passing an unknown one produces
        # Ninja's own "unknown target, did you mean ..." message.
        report_toolchain
        echo ""
        configure
        "$CMAKE" --build "$SCRIPT_DIR/$BUILD_DIR" --target "$@"
        ;;
esac
