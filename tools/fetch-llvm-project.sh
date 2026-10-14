#!/usr/bin/env sh
# ============================================================================
# fetch-llvm-project.sh — fetch the pinned llvm-project, sparsely.
#
#   tools/fetch-llvm-project.sh
#
# WHY A STANDALONE SCRIPT
#   The C and C++ SDKs are built from LLVM's libc/libcxx, and upstream is
#   ~1.9 GB, so it is not committed. It has to be fetched once on every machine
#   that builds the SDK — including a Linux CI runner.
#
#   This logic used to live inside build.sh, which is MSYS2-specific (it hunts
#   for an MSYS2 root and rewrites PATH). That made the fetch unreachable on
#   Linux, so CI had no supported way to provision the SDK. Extracting it here
#   gives one implementation that both callers share:
#
#     build.sh                 -> ./build.sh setup
#     CI / any Linux host      -> tools/fetch-llvm-project.sh
#
# WHY SPARSE
#   Only libc, libcxx, libcxxabi, libunwind, cmake, runtimes and llvm/cmake are
#   read by the build — about 137 MB of working tree instead of 1.9 GB.
#
# WHY --filter=blob:none --no-checkout FIRST
#   It fetches the commit graph and trees but no file contents, so the sparse
#   filter applies before anything large is downloaded. A plain `git clone`
#   followed by `sparse-checkout set` would still download the full pack.
#
# WHY THE CONFIG FILES ARE COPIED
#   libc/config/baremetal/x86_64/ and the x86_64-pc-none-elf cache are OURS;
#   upstream ships no x86_64 baremetal variant. They live in
#   third_party/stdlib/kyuzen/ (tracked) and are copied into the (untracked)
#   clone here, so a re-clone cannot destroy them.
#
# Idempotent: an existing clone is detected and left alone.
# ============================================================================
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

LLVM_REPO="https://github.com/llvm/llvm-project.git"
LLVM_TAG="llvmorg-22.1.8"
# The commit the project's parity baseline was built with. Same as LLVM_TAG —
# the tag is annotated, so `git rev-parse llvmorg-22.1.8^{commit}` resolves to
# this. Kept explicit so the checkout cannot silently follow a moved tag.
LLVM_COMMIT="ca7933e47d3a3451d81e72ac174dcb5aa28b59d1"
# Overridable so the fetch path can be exercised against a scratch directory.
LLVM_DIR="${KYUZEN_LLVM_DIR:-$ROOT/third_party/stdlib/llvm-project}"
KYUZEN_LLVM_CFG="$ROOT/third_party/stdlib/kyuzen"

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
    _src="$KYUZEN_LLVM_CFG/libc"
    _dst="$LLVM_DIR/libc"
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

if [ -f "$LLVM_DIR/runtimes/CMakeLists.txt" ]; then
    echo "llvm-project: already present."
    install_kyuzen_libc_config
    exit 0
fi

if [ -e "$LLVM_DIR" ] && [ ! -d "$LLVM_DIR/.git" ]; then
    echo "llvm-project: $LLVM_DIR exists but is not a git checkout." >&2
    echo "  Move it aside, then re-run this script." >&2
    exit 1
fi

# A directory that IS a git checkout but has no runtimes/ is a partial or broken
# clone (a failed sparse checkout, or a leftover from a previous run). Move it
# aside rather than deleting it: it is a 137 MB download, and deleting a
# directory the user may still want is not this script's call.
if [ -d "$LLVM_DIR/.git" ]; then
    _aside="${LLVM_DIR}.incomplete.$$"
    echo "llvm-project: existing checkout is incomplete (no runtimes/CMakeLists.txt)."
    echo "  Moving it to ${_aside##*/} and fetching a fresh one."
    echo "  Delete that directory once the build works."
    mv "$LLVM_DIR" "$_aside" || {
        echo "llvm-project: could not move the incomplete checkout aside." >&2
        exit 1
    }
fi

echo "llvm-project: not present. Fetching sparsely (this takes a few minutes)."
echo "  repo    : $LLVM_REPO"
echo "  tag     : $LLVM_TAG  ($LLVM_COMMIT)"
echo "  paths   : $LLVM_PATHS"
echo "  ~137 MB working tree + .git instead of ~1.9 GB for a full clone."
echo ""

mkdir -p "$(dirname "$LLVM_DIR")"

# 1. Trees only — no file contents yet, so the sparse filter saves the bulk of
#    the download.
if ! git clone --filter=blob:none --no-checkout --depth 1 \
               --branch "$LLVM_TAG" "$LLVM_REPO" "$LLVM_DIR"; then
    echo "llvm-project: clone failed. Check network access to GitHub." >&2
    exit 1
fi

# 2. Restrict the checkout, then materialise it.
git -C "$LLVM_DIR" sparse-checkout init --cone
git -C "$LLVM_DIR" sparse-checkout set $LLVM_PATHS
git -C "$LLVM_DIR" checkout "$LLVM_TAG"

# 3. Pin and verify. A moved or mis-resolved tag would silently change the libc
#    the whole SDK is built from.
_head="$(git -C "$LLVM_DIR" rev-parse HEAD)"
if [ "$_head" != "$LLVM_COMMIT" ]; then
    echo "llvm-project: WRONG COMMIT after checkout." >&2
    echo "  expected $LLVM_COMMIT" >&2
    echo "  got      $_head" >&2
    exit 1
fi
echo "  checked out $LLVM_TAG at ${_head%"${_head#????????}"}..."

install_kyuzen_libc_config
echo "llvm-project: done."
