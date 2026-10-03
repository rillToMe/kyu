#!/usr/bin/env python3
# _clock_tabs.py - verifikasi bahwa KEEMPAT halaman Clock benar-benar berbeda.
#
# MENGAPA ADA: probe pertama saya melaporkan "ALL PASS" padahal hasilnya salah.
# Penyebabnya dua hal yang harus dihindari di probe apa pun:
#   1. ambang "uniq>40" terlalu longgar - wallpaper desktop sendiri punya
#      puluhan ribu warna, jadi jendela kosong pun lolos;
#   2. `mouse_move` RELATIF di bawah -display none tidak memetakan 1:1 ke
#      koordinat window, jadi klik navigasi saya tidak mengenai tab.
#
# Probe ini memakai ukuran yang tidak bisa dibohongi: DIFF piksel antara
# halaman. Kalau dua halaman identik, tab tidak berpindah.
#
# Navigasi tab dijalankan lewat KEYBOARD (klik strip untuk fokus, lalu panah
# kanan). Tab sudah fokus-able dan menangani panah sendiri.
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _ui_probe as P
import _screen_text as ST

P.QEMU = r"E:\Tools\msys2\ucrt64\bin\qemu-system-x86_64.exe"
OUT = os.path.join(HERE, "_ui_out")
_ISO = os.path.join(P.ROOT, "build", "target", "boot_image.iso")
if os.path.exists(_ISO):
    P.ISO = _ISO

REPORT = os.path.join(P.ROOT, "clock_tabs.txt")
lines = []


def say(s):
    lines.append(s)
    print(s)


def wait_new(pos, needle, timeout=60.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in P.serial_text()[pos:]:
            return True
        time.sleep(0.4)
    return False


def boot_and_login(m):
    if not P.wait_serial("ready", 180):
        return False
    pos = len(P.serial_text())
    if not wait_new(pos, "Username", 60):
        return False
    m.type("root", wait=0.2); m.key("ret")
    if not wait_new(pos, "Password", 30):
        return False
    time.sleep(0.5)
    m.type("1", wait=0.2); m.key("ret")
    if not wait_new(pos, "kyuzen>", 90):
        return False
    t0 = time.time()
    while time.time() - t0 < 180:
        if P.wait_serial("[desktop] shell started", 20):
            break
        time.sleep(1.0)
    else:
        return False
    time.sleep(2.0)
    return True


def load(path):
    try:
        w, h, px = ST.read_ppm(path)
        return w, h, px
    except Exception:
        return None


def diff_frac(pa, pb):
    """Fraksi piksel yang berbeda antara dua PPM (sampling)."""
    A = load(pa); B = load(pb)
    if not A or not B or (A[0], A[1]) != (B[0], B[1]):
        return None
    w, h, a = A; _, _, b = B
    nd = 0; tot = 0
    for y in range(100, 720, 3):
        base = y * w * 3
        for x in range(100, 640, 3):
            i = base + x * 3
            tot += 1
            if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]:
                nd += 1
    return nd / float(max(1, tot))


def main():
    os.makedirs(OUT, exist_ok=True)
    P.prep()
    proc = P.start()
    mon = None
    ok_all = True
    try:
        mon = P.Mon(P.connect(timeout=40))
        time.sleep(3)
        if not boot_and_login(mon):
            say("FAIL boot/login"); return 1
        say("OK boot+login")

        pos = len(P.serial_text())
        mon.type("start clock", wait=0.2); mon.key("ret")
        if not wait_new(pos, "start: clock.elf", 25):
            say("FAIL clock tidak di-spawn"); return 1
        time.sleep(4.0)

        shots = []
        for i, name in enumerate(["p0_clock", "p1_world", "p2_stopwatch", "p3_timer"]):
            if i > 0:
                mon.key("right")
                time.sleep(2.2)
            p = os.path.join(OUT, "tabs_%s.ppm" % name)
            mon.cmd("screendump %s" % p, 2.0)
            shots.append(p)
            say("dumped %s" % name)

        # Tiap halaman harus BERBEDA dari halaman sebelumnya.
        for i in range(1, len(shots)):
            f = diff_frac(shots[i-1], shots[i])
            if f is None:
                say("FAIL %s: screendump tak terbaca" % shots[i]); ok_all = False
                continue
            say("diff p%d -> p%d = %.4f" % (i-1, i, f))
            if f < 0.01:
                say("FAIL halaman %d identik dengan sebelumnya (tab tidak pindah)" % i)
                ok_all = False
            else:
                say("PASS halaman %d berbeda (%.2f%% piksel)" % (i, f * 100))

        # Halaman 0 harus berbeda dari halaman 3 juga (bukan cuma berurutan).
        f = diff_frac(shots[0], shots[3])
        if f is not None:
            say("diff p0 -> p3 = %.4f" % f)
            if f < 0.01:
                say("FAIL halaman 0 dan 3 identik"); ok_all = False

        # Tidak ada panic.
        txt = P.serial_text()
        bad = [m for m in ("KERNEL PANIC", "EXCEPTION", "BSOD", "[P1] lockdown")
               if m in txt]
        if bad:
            say("FAIL panic: %s" % ", ".join(bad)); ok_all = False
        else:
            say("PASS tanpa panic")

        say("")
        say("TABS PROBE: %s" % ("ALL PASS" if ok_all else "FAIL"))
        return 0 if ok_all else 1
    finally:
        try:
            if mon: mon.cmd("quit")
        except Exception:
            pass
        time.sleep(1)
        try: proc.terminate()
        except Exception: pass
        with open(REPORT, "w") as f:
            f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    sys.exit(main())
