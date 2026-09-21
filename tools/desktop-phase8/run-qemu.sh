#!/usr/bin/env bash
# ============================================================
# KyuzenOS — boot desktop di QEMU (otomatis, headless)
#
# Alur (tanpa interaksi manusia):
#   1. disk image BARU (alat first-boot: password root + login shell)
#   2. keystroke dikirim lewat monitor QEMU (HMP `sendkey`) — hanya setelah
#      penanda yang relevan muncul di serial, bukan berbasis timer buta
#   3. login otomatis men-spawn desktop (apps/login.c) — TANPA perintah
#      `start`, kecuali KYUZEN_TEST_START diisi (mis. "dtsmoke" untuk smoke
#      konsol framework yang berjalan berdampingan dengan desktop)
#   4. bukti diambil dari COM1 (konsol di-mirror ke serial): marker startup
#      desktop + echo hidup dari shell (sistem tidak panic/hang)
#
# disk.img milik user TIDAK disentuh. QEMU selalu dimatikan oleh watchdog
# skrip ini (tidak menggantung walau salah satu penanda tak pernah muncul).
#
# Dipakai oleh `make desktop-qemu` / `desktop-qemu-test` / `desktop-smoke`.
#
# Override: KYUZEN_TEST_PASS, KYUZEN_TEST_START (kosong = tanpa `start`),
#   KYUZEN_TEST_NIC (default user,model=e1000; "none" bila user-mode
#   networking host macet — mis. DHCP tak pernah menjawab di Windows),
#   KYUZEN_TEST_BOOT_TIMEOUT, KYUZEN_TEST_STEP_TIMEOUT, KYUZEN_TEST_WATCHDOG,
#   KYUZEN_TEST_MKFS=1, KYUZEN_TEST_DISK/SERIAL/MONLOG/MARKER/SUCCESS/FAILURE
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUT="build/libc/desktop-phase8"
DISK="${KYUZEN_TEST_DISK:-$OUT/desktop-disk.img}"
SERIAL="${KYUZEN_TEST_SERIAL:-$OUT/desktop-serial.log}"
MONLOG="${KYUZEN_TEST_MONLOG:-$OUT/desktop-qemu-monitor.log}"
ISO="build/boot_image.iso"
MARKER="${KYUZEN_TEST_MARKER:-[desktop]}"
SUCCESS="${KYUZEN_TEST_SUCCESS:-[desktop] shell started (libdesktop)}"
FAILURE="${KYUZEN_TEST_FAILURE:-PANIC}"
START_CMD="${KYUZEN_TEST_START:-}"

ROOT_PASS="${KYUZEN_TEST_PASS:-kz}"
NIC="${KYUZEN_TEST_NIC:-user,model=e1000}"
BOOT_TIMEOUT="${KYUZEN_TEST_BOOT_TIMEOUT:-300}"
STEP_TIMEOUT="${KYUZEN_TEST_STEP_TIMEOUT:-180}"
WATCHDOG="${KYUZEN_TEST_WATCHDOG:-600}"

# --- QEMU: temukan biner yang benar-benar bisa dieksekusi ---
# $QEMU dari Makefile hanya nama ("qemu-system-x86_64.exe") yang belum tentu
# ada di PATH; verifikasi dulu sebelum dipakai, lalu coba lokasi toolchain
# msys2 yang dipakai repo ini.
QEMU_BIN="${QEMU:-}"
if [ -n "$QEMU_BIN" ]; then
    if ! command -v "$QEMU_BIN" >/dev/null 2>&1 && [ ! -x "$QEMU_BIN" ]; then
        QEMU_BIN=""
    fi
fi
if [ -z "$QEMU_BIN" ] && command -v qemu-system-x86_64.exe >/dev/null 2>&1; then
    QEMU_BIN="qemu-system-x86_64.exe"
fi
for cand in /e/Tools/msys2/qemu/qemu-system-x86_64.exe \
            /e/Tools/msys2/mingw64/bin/qemu-system-x86_64.exe \
            /e/Tools/msys2/ucrt64/bin/qemu-system-x86_64.exe; do
    if [ -z "$QEMU_BIN" ] && [ -x "$cand" ]; then
        QEMU_BIN="$cand"
    fi
done
if [ -z "$QEMU_BIN" ]; then
    echo "[qemu] FATAL: qemu-system-x86_64.exe tidak ditemukan (set QEMU=...)" >&2
    exit 1
fi

[ -f "$ISO" ] || { echo "[qemu] FATAL: $ISO tidak ada" >&2; exit 1; }
[ -x ./mkfs.kyuzenfs ] || { echo "[qemu] FATAL: mkfs.kyuzenfs tidak ada (make mkfs)" >&2; exit 1; }

# --- disk image segar supaya alur first-boot bisa dikendalikan skrip ---
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
    type_line "$ROOT_PASS"
    wait_for "[OK] File users.sys dibuat!" "$STEP_TIMEOUT" || return 1
    wait_for "Username : " "$STEP_TIMEOUT" || return 1
    type_line "root"
    wait_for "Password : " "$STEP_TIMEOUT" || return 1
    type_line "$ROOT_PASS"
    wait_for "@kyuzen>" "$STEP_TIMEOUT" || return 1
    # Desktop di-spawn otomatis oleh login; perintah `start` opsional saja.
    if [ -n "$START_CMD" ]; then
        type_line "start $START_CMD"
    fi
    # Tunggu marker sukses (desktop startup / smoke PASS / shutdown ok).
    for _ in $(seq 1 90); do
        grep -qF "$SUCCESS" "$SERIAL" 2>/dev/null && break
        grep -qF "$FAILURE" "$SERIAL" 2>/dev/null && break
        sleep 2
    done
    grep -qF "$SUCCESS" "$SERIAL" 2>/dev/null || return 1
    # Bukti hidup: shell merespons setelah desktop berjalan (tanpa hang).
    type_line "echo aliveok"
    wait_for "aliveok" "$STEP_TIMEOUT" || return 1
    return 0
}

if [ "$NIC" = "none" ]; then
    NIC_ARGS="-nic none"
else
    NIC_ARGS="-nic $NIC"
fi
echo "[qemu] $QEMU_BIN — disk segar $DISK, marker $MARKER"
# shellcheck disable=SC2086
"$QEMU_BIN" -cpu max -m 1G -boot d -smp 4 -no-reboot \
    -drive file="$DISK",format=raw,index=0,media=disk \
    -drive file="$ISO",media=cdrom,index=2 \
    $NIC_ARGS \
    -vga none -device virtio-vga,xres=1920,yres=1080 \
    -display none \
    -serial "file:$SERIAL" \
    -monitor stdio >"$MONLOG" 2>&1 < <(feed_keys) &
QPID=$!

deadline=$((SECONDS + WATCHDOG))
while kill -0 "$QPID" 2>/dev/null; do
    if grep -qF -- "$FAILURE" "$SERIAL" 2>/dev/null; then
        echo "[qemu] penanda FAILURE terlihat: $FAILURE" >&2
        sleep 4
        break
    fi
    if grep -qF -- "$SUCCESS" "$SERIAL" 2>/dev/null && grep -qF "aliveok" "$SERIAL" 2>/dev/null; then
        echo "[qemu] sukses + shell hidup — QEMU dimatikan" >&2
        sleep 4
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
    taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
fi
wait "$QPID" 2>/dev/null

echo "[qemu] selesai; serial: $SERIAL"
if grep -qF -- "$FAILURE" "$SERIAL"; then
    echo "[qemu] FAIL: penanda failure '$FAILURE' muncul di serial" >&2
    tail -40 "$SERIAL" >&2 || true
    exit 1
fi
if grep -qF -- "$SUCCESS" "$SERIAL" && grep -qF "aliveok" "$SERIAL"; then
    echo "[qemu] sukses: '$SUCCESS' + shell hidup (aliveok)"
    exit 0
fi

echo "[qemu] FAIL: '$SUCCESS' tidak muncul di serial" >&2
echo "--- 40 baris terakhir serial ---" >&2
tail -40 "$SERIAL" >&2 || true
exit 1
