#!/usr/bin/env bash
# Guard isolasi desktop Phase 8 (gagal keras bila boundary bocor).
#
# 1. Header publik (libs/gui/libdesktop/include) hanya boleh memakai:
#      <stdint.h>, <kyuzen/desktop/...> — tanpa userlib.h/libgui.h,
#      tanpa path privat (kernel/, libs/*/src, apps/, system/, libs/core/,
#      third_party/, build/), tanpa simbol ABI mentah.
# 2. Backend framework (libs/gui/libdesktop/src) tidak boleh memakai kode
#    implementasi desktop (system/desktop, tests/target/test-desktop).
# 3. Implementasi desktop tidak boleh memakai internal framework
#    (libs/gui/libdesktop/src), kernel, third_party, build, tests/,
#    atau syscall inline mentah.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

fail=0
bad() { echo "[desktop-isolation] FAIL: $1" >&2; fail=1; }

PUB="libs/gui/libdesktop/include/kyuzen/desktop"
if [ ! -d "$PUB" ]; then
    echo "[desktop-isolation] FAIL: $PUB tidak ada" >&2
    exit 1
fi

# 1a. Include yang diizinkan di header publik.
while IFS= read -r inc; do
    case "$inc" in
        stdint.h|kyuzen/desktop/*) ;;
        *) bad "include tak diizinkan di header publik: $inc" ;;
    esac
done < <(grep -rhoE '#include [<"][^>"]+[>"]' "$PUB" | sed -E 's/#include [<"]//; s/[>"]//' | sort -u)

# 1b. Simbol/path privat di header publik.
if grep -rnE "userlib\.h|libgui\.h|color_types\.h|crash_notice\.h|kwm\.h|proc\.h" "$PUB"; then
    bad "header publik menyebut header C privat"
fi
# Pola path privat: "apps/"/"system/"/"lib/" sebagai segmen path sumber
# (system/desktop, tests, ...), BUKAN path runtime FS "/apps/..." yang
# dipakai launcher/impl secara sah.
PRIV_PATH='userlib\.h|libgui\.h|color_types\.h|crash_notice\.h|kwm\.h|proc\.h|([^/]|^)(apps|system|lib)/|([^/]|^)kernel/|third_party/|([^/]|^)build/|sys_get_event|sys_kwm_|sys_crash_notice|kyuzen_event_t|kwm_window_info_t|crash_notice_t|gui_window_t|gui_'
if grep -rnE "$PRIV_PATH" "$PUB"; then
    bad "header publik bocor ke privat/ABI mentah"
fi

# 2. Backend bebas policy implementasi.
if grep -rnE "system/desktop|tests/target/test-desktop|Launcher|Taskbar|CrashNotice|DesktopShell|TestShell" libs/gui/libdesktop/src; then
    bad "backend framework menyebut implementasi desktop"
fi
if grep -rnE "third_party/|build/|tests/" libs/gui/libdesktop/src; then
    bad "backend framework bocor ke third_party/build/tests"
fi

# 3. Implementasi = userspace biasa (boleh wrapper C SDK, bukan mentah).
for d in system/desktop tests/target/test-desktop tools/desktop-phase8; do
    [ -d "$d" ] || continue
    if grep -rnE "libs/gui/libdesktop/src|third_party/|tests/|kernel/|__asm__|int \$0x80|syscall\(" "$d" --include="*.hpp" --include="*.cpp" | grep -v "sys_abi.hpp:.*userlib"; then
        bad "implementasi $d bocor ke privat/mentah"
    fi
done
# sys_abi.hpp adalah satu-satunya wrap yang menyebut userlib.h di sisi impl
# (hanya sumber C++ yang diperiksa — README/prose dikecualikan).
if [ "$(grep -rl 'userlib\.h' system/desktop tests/target/test-desktop --include="*.hpp" --include="*.cpp" 2>/dev/null | grep -v sys_abi.hpp | wc -l)" != "0" ]; then
    bad "userlib.h di-include di luar sys_abi.hpp"
fi

if [ "$fail" = "0" ]; then
    echo "[desktop-isolation] 0 forbidden desktop references"
    exit 0
fi
exit 1
