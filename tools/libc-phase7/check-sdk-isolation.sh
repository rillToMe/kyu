#!/usr/bin/env bash
# Kyuzen C++ SDK isolation guard (Phase 7).
#
# Memastikan source aplikasi publik tidak merujuk langsung ke implementasi
# privat: tree LLVM, port libc, atau direktori build internal. SDK sendiri
# boleh memakai path itu; file yang diperiksa di sini tidak boleh.
set -euo pipefail

if [ $# -eq 0 ]; then
  echo "Usage: check-sdk-isolation.sh <app-source-or-dir>..." >&2
  exit 2
fi

patterns=(
  "third_party/stdlib/llvm-project"
  "libs/c/libc-port"
  "build/libc"
  "build/libcxx"
)

matches=0
for target in "$@"; do
  for pattern in "${patterns[@]}"; do
    count="$(grep -R -F -- "$pattern" "$target" 2>/dev/null | wc -l || true)"
    count="$(printf '%s' "$count" | tr -d '[:space:]')"
    if [ "$count" -gt 0 ]; then
      echo "[sdk-isolation] FAIL: $count rujukan '$pattern' di $target" >&2
      grep -R -F -- "$pattern" "$target" >&2 || true
      matches=$((matches + count))
    fi
  done
done

echo "[sdk-isolation] $matches forbidden application references"
[ "$matches" -eq 0 ]
