#!/usr/bin/env bash
# ============================================================
# KyuzenOS - smoke test Lexbor Stage A di QEMU (otomatis, headless)
#
# Alur (tanpa interaksi manusia):
#   1. disk image BARU (build/libc/lexbor-a-disk.img) - boot pertama
#      meminta password root baru (system/login.c: first_time_setup)
#   2. keystroke dikirim lewat monitor QEMU (HMP `sendkey`) - hanya setelah
#      penanda serial muncul, bukan timer buta
#   3. app dijalankan dari console shell: `start lexbor_phase_a`
#   4. bukti diambil dari COM1 (drivers/tty.c me-mirror TTY ke serial):
#      "[lexbor-a] PASS" = parse HTML -> DOM lookup -> serialize -> malformed
#      -> empty, semuanya di userspace Kyuzen (allocator = libc Kyuzen).
#
# disk.img milik user TIDAK disentuh. QEMU selalu dimatikan oleh watchdog.
#
# Override opsional: KYUZEN_TEST_PASS, KYUZEN_TEST_APP, KYUZEN_TEST_BOOT_TIMEOUT,
#                    KYUZEN_TEST_STEP_TIMEOUT, KYUZEN_TEST_WATCHDOG,
#                    KYUZEN_TEST_MKFS=1 (format lewat mkfs host, default TIDAK)
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUT="build/libc/lexbor-stage-a"
DISK="$OUT/lexbor-a-disk.img"
SERIAL="$OUT/lexbor-a-serial.log"
MONLOG="$OUT/lexbor-a-qemu-monitor.log"
ISO="build/boot_image.iso"
APP="$OUT/x86_64-pc-none-elf/bin/lexbor_phase_a.elf"

ROOT_PASS="${KYUZEN_TEST_PASS:-kz}"
APP_NAME="${KYUZEN_TEST_APP:-lexbor_phase_a}"
BOOT_TIMEOUT="${KYUZEN_TEST_BOOT_TIMEOUT:-300}"
STEP_TIMEOUT="${KYUZEN_TEST_STEP_TIMEOUT:-120}"
WATCHDOG="${KYUZEN_TEST_WATCHDOG:-420}"

# --- QEMU: PATH dulu, lalu lokasi toolchain msys2 yang dipakai repo ini ---
QEMU_BIN="${QEMU:-}"
if [ -z "$QEMU_BIN" ] && command -v qemu-system-x86_64.exe >/dev/null 2>&1; then
    QEMU_BIN="qemu-system-x86_64.exe"
fi
if [ -z "$QEMU_BIN" ] && [ -x /e/Tools/msys2/mingw64/bin/qemu-system-x86_64.exe ]; then
    QEMU_BIN="/e/Tools/msys2/mingw64/bin/qemu-system-x86_64.exe"
fi
if [ -z "$QEMU_BIN" ] && [ -x /e/Tools/msys2/qemu/qemu-system-x86_64.exe ]; then
    QEMU_BIN="/e/Tools/msys2/qemu/qemu-system-x86_64.exe"
fi
if [ -z "$QEMU_BIN" ]; then
    echo "[qemu] FATAL: qemu-system-x86_64.exe tidak ditemukan (set QEMU=...)" >&2
    exit 1
fi

for f in "$ISO" "$APP"; do
    [ -f "$f" ] || { echo "[qemu] FATAL: $f tidak ada (make lexbor-stage-a-qemu yang menyiapkannya)" >&2; exit 1; }
done
[ -x ./mkfs.kyuzenfs ] || { echo "[qemu] FATAL: mkfs.kyuzenfs tidak ada (make mkfs)" >&2; exit 1; }

# --- disk image segar; default image NOL mentah (kernel memformat sendiri) ---
mkdir -p "$OUT"
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=64 2>/dev/null
if [ "${KYUZEN_TEST_MKFS:-0}" = "1" ]; then
    ./mkfs.kyuzenfs "$DISK" >/dev/null
fi
rm -f "$SERIAL" "$MONLOG"
: > "$SERIAL"

# ------------------------------------------------------------
# Pengirim keystroke ke monitor QEMU (stdin QEMU diisi dari fungsi ini).
# ------------------------------------------------------------
key() { echo "sendkey $1"; sleep 0.06; }

type_word() {
    local s="$1" i c
    for ((i = 0; i < ${#s}; i++)); do
        c="${s:i:1}"
        case "$c" in
            [a-z0-9]) key "$c" ;;
            _)        key "shift-minus" ;;
            -)        key "minus" ;;
            .)        key "dot" ;;
            *)        key "spc" ;;
        esac
    done
}
type_line() { type_word "$1"; key "ret"; }

wait_for() {
    local pat="$1" timeout="$2" waited=0
    while [ "$waited" -lt "$timeout" ]; do
        if grep -qF -- "$pat" "$SERIAL" 2>/dev/null; then
            echo "[qemu] marker: $pat" >&2
            return 0
        fi
        sleep 2
        waited=$((waited + 2))
    done
    echo "[qemu] TIMEOUT menunggu marker: $pat" >&2
    return 1
}

feed_keys() {
    wait_for "Password Root Baru : " "$BOOT_TIMEOUT" || return 1
    type_line "$ROOT_PASS"
    wait_for "[OK] File users.sys dibuat!" "$STEP_TIMEOUT" || return 1
    wait_for "Username : " "$STEP_TIMEOUT" || return 1
    type_line "root"
    wait_for "Password : " "$STEP_TIMEOUT" || return 1
    type_line "$ROOT_PASS"
    wait_for "@kyuzen>" "$STEP_TIMEOUT" || return 1
    type_line "start $APP_NAME"
    for _ in $(seq 1 120); do
        grep -qF "[lexbor-a] PASS" "$SERIAL" 2>/dev/null && return 0
        grep -qF "[lexbor-a] FAIL" "$SERIAL" 2>/dev/null && return 0
        sleep 2
    done
    return 1
}

echo "[qemu] $QEMU_BIN - disk segar $DISK, app $APP_NAME"
"$QEMU_BIN" -cpu max -m 1G -boot d -smp 4 -no-reboot \
    -drive file="$DISK",format=raw,index=0,media=disk \
    -drive file="$ISO",media=cdrom,index=2 \
    -nic user,model=e1000 \
    -vga none -device virtio-vga,xres=1920,yres=1080 \
    -display none \
    -serial "file:$SERIAL" \
    -monitor stdio >"$MONLOG" 2>&1 < <(feed_keys) &
QPID=$!

deadline=$((SECONDS + WATCHDOG))
while kill -0 "$QPID" 2>/dev/null; do
    if grep -qE "\[lexbor-a\] (PASS|FAIL)" "$SERIAL" 2>/dev/null; then
        sleep 4
        break
    fi
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "[qemu] watchdog ${WATCHDOG}s habis - QEMU dimatikan" >&2
        break
    fi
    sleep 3
done

kill "$QPID" 2>/dev/null
sleep 2
if kill -0 "$QPID" 2>/dev/null; then
    kill -9 "$QPID" 2>/dev/null
    sleep 1
fi
if kill -0 "$QPID" 2>/dev/null; then
    taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
fi
wait "$QPID" 2>/dev/null

echo "[qemu] selesai; serial: $SERIAL"
if grep -qF "[lexbor-a] PASS" "$SERIAL"; then
    echo "[qemu] PASS terlihat di serial"
    exit 0
fi

echo "[qemu] FAIL: '[lexbor-a] PASS' tidak muncul di serial" >&2
echo "--- 40 baris terakhir serial ---" >&2
tail -40 "$SERIAL" >&2 || true
exit 1
