#!/usr/bin/env bash
# ============================================================
# KyuzenOS — Font Demo QEMU visual test (headless, otomatis).
#
# Alur (pola tools/libc-phase1/run-qemu.sh):
#   1. salinan disk.img (akun root/1 sudah ada — TANPA setup baru)
#   2. login root/1 lewat monitor sendkey (dipandu marker serial)
#   3. `start fontdemo` dari shell, tunggu "[fontdemo] launch"
#   4. klik tengah window (mouse_move/button) untuk fokus
#   5. Right x4 (5 font) + screendump tiap font + Left x1
#   6. `quit` lewat monitor; bukti = serial + 6 PPM
#
# QEMU dimatikan skrip ini (watchdog), tidak menggantung.
# Dipakai: bash tools/fontdemo/run-qemu.sh [QEMU=...]
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUT="build/fontdemo"
DISK="$OUT/fontdemo-disk.img"
SERIAL="$OUT/fontdemo-serial.log"
MONLOG="$OUT/fontdemo-monitor.log"
ISO="build/boot_image.iso"

BOOT_TIMEOUT="${KYUZEN_TEST_BOOT_TIMEOUT:-300}"
STEP_TIMEOUT="${KYUZEN_TEST_STEP_TIMEOUT:-120}"
WATCHDOG="${KYUZEN_TEST_WATCHDOG:-600}"

QEMU_BIN="${QEMU:-}"
if [ -z "$QEMU_BIN" ] && command -v qemu-system-x86_64.exe >/dev/null 2>&1; then
    QEMU_BIN="qemu-system-x86_64.exe"
fi
if [ -z "$QEMU_BIN" ] && [ -x /e/Tools/msys2/qemu/qemu-system-x86_64.exe ]; then
    QEMU_BIN="/e/Tools/msys2/qemu/qemu-system-x86_64.exe"
fi
if [ -z "$QEMU_BIN" ]; then
    echo "[qemu] FATAL: qemu tidak ditemukan (set QEMU=...)" >&2
    exit 1
fi

[ -f "$ISO" ] || { echo "[qemu] FATAL: $ISO tidak ada" >&2; exit 1; }
[ -f "disk.img" ] || { echo "[qemu] FATAL: disk.img tidak ada" >&2; exit 1; }

mkdir -p "$OUT"
cp -f disk.img "$DISK"
rm -f "$SERIAL" "$MONLOG"
: > "$SERIAL"

key() { echo "sendkey $1"; sleep 0.06; }

type_word() {
    local s="$1" i c
    for ((i = 0; i < ${#s}; i++)); do
        c="${s:i:1}"
        case "$c" in
            [a-z0-9]) key "$c" ;;
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

shot() {
    echo "screendump $OUT/shot$1.ppm"
    sleep 2
}

# Tunggu kemunculan marker ke-N (1-based) — pola wait_for langsung
# cocok seketika bila marker sudah ada dari font sebelumnya.
wait_count() {
    local pat="$1" want="$2" timeout="$3" waited=0 n=0
    while [ "$waited" -lt "$timeout" ]; do
        n=$(grep -cF -- "$pat" "$SERIAL" 2>/dev/null || echo 0)
        if [ "$n" -ge "$want" ]; then
            echo "[qemu] marker x$want: $pat" >&2
            return 0
        fi
        sleep 2
        waited=$((waited + 2))
    done
    echo "[qemu] TIMEOUT menunggu marker x$want: $pat" >&2
    return 1
}

feed_keys() {
    wait_for "Username : " "$BOOT_TIMEOUT" || return 1
    type_line "root"
    wait_for "Password : " "$STEP_TIMEOUT" || return 1
    type_line "1"
    wait_for "@kyuzen>" "$STEP_TIMEOUT" || return 1
    sleep 2
    type_line "start fontdemo"
    wait_for "[fontdemo] launch" "$STEP_TIMEOUT" || return 1
    sleep 4
    # fokus window demo (klik tengah area window 660x500 di ~(100,80))
    echo "mouse_move 400 300"
    sleep 1
    echo "mouse_button 1"
    sleep 0.3
    echo "mouse_button 0"
    sleep 2
    shot 0                                    # font 0 DejaVu (lookups #1)
    for i in 1 2 3 4; do
        key "right"
        # tiap font mencetak cache16 lookups sekali setelah render pertama
        wait_count "cache16 lookups" "$((i + 1))" "$STEP_TIMEOUT" || return 1
        sleep 2
        shot "$i"
    done
    key "left"                                # kembali ke font 3
    wait_count "cache16 lookups" 6 "$STEP_TIMEOUT" || return 1
    sleep 3
    shot 5
    sleep 2
    echo "quit"
    return 0
}

echo "[qemu] $QEMU_BIN — disk salinan $DISK"
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
    if grep -qF "[fontdemo] font " "$SERIAL" 2>/dev/null; then
        sleep 1
    fi
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "[qemu] watchdog ${WATCHDOG}s habis — QEMU dimatikan" >&2
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
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

echo "[qemu] selesai; serial: $SERIAL shots: $OUT/shot*.ppm"
ls -la "$OUT"/shot*.ppm 2>/dev/null || echo "[qemu] TIDAK ADA screenshot"
grep -h "\[fontdemo\]" "$SERIAL" 2>/dev/null || echo "[qemu] TIDAK ADA marker fontdemo"
