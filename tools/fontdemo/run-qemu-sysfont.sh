#!/usr/bin/env bash
# ============================================================
# KyuzenOS — System Font Policy QEMU test (headless, otomatis).
#
# 1. boot + login root/1 (disk salinan, akun existing)
# 2. `start settings` -> "[settings] font ui Inter Regular" (default)
# 3. shot A (desktop label Inter) — settings window menutupi tengah;
#    label desktop tetap terlihat di kiri (17 ikon, sel 0..16)
# 4. kirim '2' (DejaVu, live preview) + shot B (window settings)
# 5. kirim 's' (simpan font.ui) -> tunggu "[desktop] uifont DejaVu Sans"
#    (poll ≤5 dtk) -> shot C (label desktop DejaVu)
# 6. kirim '1' + 's' (kembali Inter) -> tunggu marker -> shot D
# 7. quit. Bukti = serial + 4 PPM. Watchdog mematikan QEMU.
# ============================================================
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
OUT="build/fontdemo"
DISK="$OUT/sysfont-disk.img"
SERIAL="$OUT/sysfont-serial.log"
MONLOG="$OUT/sysfont-monitor.log"
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
[ -n "$QEMU_BIN" ] || { echo FATAL >&2; exit 1; }
[ -f "$ISO" ] || { echo "FATAL: no ISO" >&2; exit 1; }
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
    echo "[qemu] TIMEOUT: $pat" >&2
    return 1
}
shot() { echo "screendump $OUT/$1"; sleep 2; }
feed_keys() {
    wait_for "Username : " "$BOOT_TIMEOUT" || return 1
    type_line "root"
    wait_for "Password : " "$STEP_TIMEOUT" || return 1
    type_line "1"
    wait_for "@kyuzen>" "$STEP_TIMEOUT" || return 1
    sleep 2
    # A: desktop label Inter (default, tanpa font.ui)
    # Pola = fragmen stabil "uifont X" (baris serial kadang terbelah oleh
    # task lain yang menulis bersamaan; prefix "[desktop]" tak dijamin utuh).
    wait_for "uifont Inter Regular" "$STEP_TIMEOUT" || return 1
    sleep 6
    shot "sys-a-inter.ppm"
    type_line "start settings"
    wait_for "font ui Inter Regular" "$STEP_TIMEOUT" || return 1
    sleep 4
    shot "sys-b-settings.ppm"
    key "2"                                            # pilih DejaVu (live)
    sleep 4
    shot "sys-c-preview.ppm"
    key "s"                                            # simpan font.ui
    wait_for "uifont DejaVu Sans" "$STEP_TIMEOUT" || return 1
    sleep 4
    shot "sys-d-dejavu.ppm"
    key "1"                                            # kembali Inter
    sleep 2
    key "s"
    wait_for "uifont Inter Regular" "$STEP_TIMEOUT" || return 1
    sleep 3
    shot "sys-e-back.ppm"
    sleep 2
    echo "quit"
    return 0
}
echo "[qemu] $QEMU_BIN sysfont test"
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
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "[qemu] watchdog habis" >&2
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
echo "[qemu] selesai"
ls -la "$OUT"/sys-*.ppm 2>/dev/null || echo NO-SHOTS
grep -h "uifont\|font ui" "$SERIAL" 2>/dev/null || echo NO-MARKER
