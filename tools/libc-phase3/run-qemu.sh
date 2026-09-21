#!/usr/bin/env bash
# ============================================================
# KyuzenOS — smoke test Kyuzen C SDK (Phase 3) di QEMU (otomatis)
#
# Alur (tanpa interaksi manusia):
#   1. disk image BARU (build/libc/phase3-disk.img) → boot pertama meminta
#      password root baru (apps/login.c: first_time_setup)
#   2. keystroke dikirim lewat monitor QEMU (HMP `sendkey`) — hanya setelah
#      penanda yang relevan muncul di serial, bukan berbasis timer buta
#   3. app dijalankan dari console shell: `start sdk_smoke hello`
#      (argv[1]=="hello" adalah bagian dari assertion aplikasi)
#   4. bukti diambil dari COM1 (drivers/tty.c me-mirror TTY ke serial)
#
# disk.img milik user TIDAK disentuh. QEMU selalu dimatikan oleh watchdog
# skrip ini (tidak menggantung walau salah satu penanda tak pernah muncul).
#
# Dipakai oleh `make sdk-c-smoke-qemu`; bisa juga dijalankan langsung:
#   QEMU=/e/Tools/msys2/qemu/qemu-system-x86_64.exe bash tools/libc-phase3/run-qemu.sh
#
# Override opsional: KYUZEN_TEST_PASS, KYUZEN_TEST_APP, KYUZEN_TEST_ARGS,
#                    KYUZEN_TEST_BOOT_TIMEOUT, KYUZEN_TEST_STEP_TIMEOUT,
#                    KYUZEN_TEST_WATCHDOG, KYUZEN_TEST_MKFS=1
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUT="build/libc"
DISK="$OUT/phase3-disk.img"
SERIAL="$OUT/phase3-serial.log"
MONLOG="$OUT/phase3-qemu-monitor.log"
ISO="build/boot_image.iso"
APP="$OUT/x86_64-pc-none-elf/bin/sdk_smoke.elf"

ROOT_PASS="${KYUZEN_TEST_PASS:-kz}"
APP_NAME="${KYUZEN_TEST_APP:-sdk_smoke}"
APP_ARGS="${KYUZEN_TEST_ARGS:-hello}"
BOOT_TIMEOUT="${KYUZEN_TEST_BOOT_TIMEOUT:-300}"
STEP_TIMEOUT="${KYUZEN_TEST_STEP_TIMEOUT:-120}"
WATCHDOG="${KYUZEN_TEST_WATCHDOG:-420}"

# --- QEMU: PATH dulu, lalu lokasi toolchain msys2 yang dipakai repo ini ---
QEMU_BIN="${QEMU:-}"
if [ -z "$QEMU_BIN" ] && command -v qemu-system-x86_64.exe >/dev/null 2>&1; then
    QEMU_BIN="qemu-system-x86_64.exe"
fi
if [ -z "$QEMU_BIN" ] && [ -x /e/Tools/msys2/qemu/qemu-system-x86_64.exe ]; then
    QEMU_BIN="/e/Tools/msys2/qemu/qemu-system-x86_64.exe"
fi
if [ -z "$QEMU_BIN" ]; then
    echo "[qemu] FATAL: qemu-system-x86_64.exe tidak ditemukan (set QEMU=...)" >&2
    exit 1
fi

for f in "$ISO" "$APP"; do
    [ -f "$f" ] || { echo "[qemu] FATAL: $f tidak ada (make sdk-c-smoke-qemu yang menyiapkannya)" >&2; exit 1; }
done
[ -x ./mkfs.kyuzenfs ] || { echo "[qemu] FATAL: mkfs.kyuzenfs tidak ada (make mkfs)" >&2; exit 1; }

# --- disk image segar supaya alur first-boot bisa dikendalikan skrip ---
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
            " ")      key "spc" ;;
            *)        key "spc" ;;
        esac
    done
}
type_line() { type_word "$1"; key "ret"; }

# Tunggu penanda (fixed string) muncul di serial.
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
    type_line "$ROOT_PASS"                              # isi password root baru
    wait_for "[OK] File users.sys dibuat!" "$STEP_TIMEOUT" || return 1
    wait_for "Username : " "$STEP_TIMEOUT" || return 1
    type_line "root"
    wait_for "Password : " "$STEP_TIMEOUT" || return 1
    type_line "$ROOT_PASS"
    wait_for "@kyuzen>" "$STEP_TIMEOUT" || return 1     # console shell siap
    type_line "start $APP_NAME $APP_ARGS"
    # Biarkan app jalan; watchdog utama yang memutuskan kapan QEMU dimatikan.
    for _ in $(seq 1 120); do
        grep -qF "[phase3] PASS" "$SERIAL" 2>/dev/null && return 0
        grep -qF "[phase3] FAIL" "$SERIAL" 2>/dev/null && return 0
        sleep 2
    done
    return 1
}

echo "[qemu] $QEMU_BIN — disk segar $DISK, app $APP_NAME $APP_ARGS"
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
    if grep -qE "\[phase3\] (PASS|FAIL)" "$SERIAL" 2>/dev/null; then
        sleep 4   # beri waktu sisa output serial tersimpan
        break
    fi
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "[qemu] watchdog ${WATCHDOG}s habis — QEMU dimatikan" >&2
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
    # Fallback Windows: paksa matikan semua qemu-system-x86_64 (aman di sini:
    # instance ini milik skrip, bukan QEMU manual milik user).
    taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
fi
wait "$QPID" 2>/dev/null

echo "[qemu] selesai; serial: $SERIAL"
if grep -qF "[phase3] PASS" "$SERIAL"; then
    echo "[qemu] PASS terlihat di serial"
    exit 0
fi

echo "[qemu] FAIL: '[phase3] PASS' tidak muncul di serial" >&2
echo "--- 40 baris terakhir serial ---" >&2
tail -40 "$SERIAL" >&2 || true
exit 1
