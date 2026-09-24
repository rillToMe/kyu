#!/usr/bin/env bash
# Homing kursor ke ikon fontdemo (1584,48) secara closed-loop, lalu klik.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
OUT="build/fontdemo"
DISK="$OUT/fontdemo-disk.img"
SERIAL="$OUT/fontdemo-home-serial.log"
MONLOG="$OUT/fontdemo-home-monitor.log"
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
[ -n "$QEMU_BIN" ] || exit 1
mkdir -p "$OUT"
cp -f disk.img "$DISK"
rm -f "$SERIAL" "$MONLOG"
: > "$SERIAL"
FIFO="$OUT/home-fifo"
rm -f "$FIFO"
mkfifo "$FIFO"
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
        if grep -qF -- "$pat" "$SERIAL" 2>/dev/null; then return 0; fi
        sleep 2
        waited=$((waited + 2))
    done
    return 1
}
# boot + login di background;进一步骤 dibaca dari FIFO oleh driver python
(
    wait_for "Username : " "$BOOT_TIMEOUT" || exit 1
    type_line "root"
    wait_for "Password : " "$STEP_TIMEOUT" || exit 1
    type_line "1"
    wait_for "@kyuzen>" "$STEP_TIMEOUT" || exit 1
    sleep 6
    echo BOOTED > "$FIFO"
    # tunggu perintah homing selesai dari driver
    while read -r line < "$FIFO"; do
        if [ "$line" = "CLICK" ]; then
            echo "mouse_move 1584 48"
            sleep 2
            echo "mouse_button 1"
            sleep 0.3
            echo "mouse_button 0"
            sleep 1
            echo "screendump $OUT/home-click.ppm"
            sleep 2
            echo DONE > "$FIFO"
            break
        else
            # "MOVE x y"
            set -- $line
            echo "mouse_move $2 $3"
            sleep 2
            echo "screendump $OUT/home-pos.ppm"
            sleep 2
            echo MOVED > "$FIFO"
        fi
    done
    if wait_for "[fontdemo] launch" "$STEP_TIMEOUT"; then
        echo "screendump $OUT/home-app.ppm"
        sleep 2
    fi
    echo "quit"
) 2> "$OUT/home-feed.log" | {
    "$QEMU_BIN" -cpu max -m 1G -boot d -smp 4 -no-reboot \
        -drive file="$DISK",format=raw,index=0,media=disk \
        -drive file="$ISO",media=cdrom,index=2 \
        -nic user,model=e1000 \
        -vga none -device virtio-vga,xres=1920,yres=1080 \
        -usb -device usb-tablet \
        -display none \
        -serial "file:$SERIAL" \
        -monitor stdio >"$MONLOG" 2>&1
} &
QPID=$!
# driver homing python (butuh E:\Tools\msys2\clang64\bin\python3.exe di PATH)
if read -r line < "$FIFO" && [ "$line" = "BOOTED" ]; then
    E:/Tools/msys2/clang64/bin/python3.exe tools/fontdemo/home_driver.py
fi
deadline=$((SECONDS + WATCHDOG))
while kill -0 "$QPID" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 3
done
kill "$QPID" 2>/dev/null; sleep 2
kill -9 "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null
rm -f "$FIFO"
echo "[qemu] selesai"
ls -la "$OUT"/home-*.ppm 2>/dev/null || echo NO-SHOTS
grep -h "\[fontdemo\]" "$SERIAL" 2>/dev/null || echo NO-MARKER
