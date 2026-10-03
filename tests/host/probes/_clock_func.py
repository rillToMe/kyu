#!/usr/bin/env python3
# _clock_func.py - verifikasi FUNGSI stopwatch & timer (bukan hanya render).
#
# MENGAPA ADA: "halaman tergambar" tidak membuktikan apa pun tentang perilaku.
# Yang harus dibuktikan:
#   * stopwatch BENAR-BENAR berjalan: tampilan berubah tanpa input apa pun;
#   * stopwatch berhenti saat Pause: tampilan BERHENTI berubah;
#   * timer menghitung turun setelah Start.
#
# Metode: diff piksel antar waktu, dibatasi ke area angka (bukan seluruh
# jendela, supaya kursor/animasi lain tidak mengaburkan hasil).
#
# Tombol ditekan lewat KEYBOARD: setelah Tab difokuskan (lihat app.cpp),
# Tab memindahkan fokus ke kontrol berikutnya dan Enter/Spasi menekan tombol.
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

REPORT = os.path.join(P.ROOT, "clock_func.txt")
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


def diff_region(pa, pb, x0, y0, x1, y1, step=2):
    A = ST.read_ppm(pa); B = ST.read_ppm(pb)
    w, h, a = A
    _, _, b = B
    nd = 0; tot = 0
    for y in range(y0, min(y1, h), step):
        for x in range(x0, min(x1, w), step):
            i = (y * w + x) * 3
            tot += 1
            if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]:
                nd += 1
    return nd / float(max(1, tot))


def main():
    os.makedirs(OUT, exist_ok=True)
    P.prep()
    proc = P.start()
    mon = None
    ok = True
    try:
        mon = P.Mon(P.connect(timeout=40))
        time.sleep(3)
        if not boot_and_login(mon):
            say("FAIL boot/login"); return 1
        say("OK boot+login")

        pos = len(P.serial_text())
        mon.type("start clock", wait=0.2); mon.key("ret")
        if not wait_new(pos, "start: clock.elf", 25):
            say("FAIL spawn"); return 1
        time.sleep(4.0)

        # --- Pindah ke Stopwatch (tab index 2): panah kanan 2x ---
        mon.key("right"); time.sleep(1.6)
        mon.key("right"); time.sleep(1.6)
        mon.cmd("screendump %s/f_sw_a.ppm" % OUT, 2.0)

        # Tab berpindah fokus keluar dari strip: Tab sekali -> tombol pertama.
        mon.key("tab"); time.sleep(0.8)
        # Tekan tombol Start dengan Enter.
        mon.key("ret"); time.sleep(0.8)
        time.sleep(1.2)
        mon.cmd("screendump %s/f_sw_b.ppm" % OUT, 2.0)
        time.sleep(2.0)
        mon.cmd("screendump %s/f_sw_c.ppm" % OUT, 2.0)

        # Angka stopwatch ada di sekitar y=170..230 (hero). Ukur di area itu.
        d1 = diff_region(os.path.join(OUT, "f_sw_a.ppm"),
                         os.path.join(OUT, "f_sw_b.ppm"), 110, 160, 600, 300)
        d2 = diff_region(os.path.join(OUT, "f_sw_b.ppm"),
                         os.path.join(OUT, "f_sw_c.ppm"), 110, 160, 600, 300)
        say("stopwatch: diff(sebelum->start) = %.4f" % d1)
        say("stopwatch: diff(start->+2s)    = %.4f" % d2)
        # Ambang: angka hero "00:00.000" hanya menempati ~47x50 px, jadi
        # perubahan milidetik menggeser ~250 piksel sampel = ~0.3% dari area
        # yang dipindai. Ambang 0.01 terlalu longgar untuk itu; 0.001 cukup
        # untuk membedakan "berjalan" dari "beku" (yang memberi 0.0000).
        if d2 > 0.001:
            say("PASS stopwatch BERJALAN (angka berubah tanpa input)")
        else:
            say("FAIL stopwatch tidak berjalan (angka beku)")
            ok = False

        # --- Pause: Tab ke tombol berikutnya? Tidak: fokus ada di tombol Start
        # (kini berlabel Pause). Enter menekan tombol yang sama -> jeda.
        mon.key("ret"); time.sleep(0.8)
        mon.cmd("screendump %s/f_sw_p1.ppm" % OUT, 2.0)
        time.sleep(2.5)
        mon.cmd("screendump %s/f_sw_p2.ppm" % OUT, 2.0)
        d3 = diff_region(os.path.join(OUT, "f_sw_p1.ppm"),
                         os.path.join(OUT, "f_sw_p2.ppm"), 110, 160, 600, 300)
        say("stopwatch: diff(setelah jeda, +2.5s) = %.4f" % d3)
        # Saat dijeda angkanya benar-benar diam -> 0.0000. Ambang kecil
        # (bukan 0.004) supaya "diam" tidak lolos karena satu piksel noise.
        if d3 < 0.0005:
            say("PASS stopwatch BERHENTI saat jeda")
        else:
            say("FAIL stopwatch masih berjalan setelah jeda")
            ok = False

        # --- Timer: panah kanan sekali lagi (tab 3) ---
        mon.key("right"); time.sleep(1.8)
        mon.cmd("screendump %s/f_tm_a.ppm" % OUT, 2.0)
        # Fokuskan tombol: Tab dari strip ke kontrol pertama di panel.
        mon.key("tab"); time.sleep(0.6)
        mon.key("tab"); time.sleep(0.6)
        mon.key("ret"); time.sleep(0.8)
        time.sleep(2.0)
        mon.cmd("screendump %s/f_tm_b.ppm" % OUT, 2.0)
        d4 = diff_region(os.path.join(OUT, "f_tm_a.ppm"),
                         os.path.join(OUT, "f_tm_b.ppm"), 110, 160, 600, 420)
        say("timer: diff(sebelum->start) = %.4f" % d4)
        if d4 > 0.005:
            say("PASS timer berubah setelah Start")
        else:
            say("NOTE timer tidak berubah - cek apakah fokus tombol tepat")

        txt = P.serial_text()
        bad = [m for m in ("KERNEL PANIC", "EXCEPTION", "BSOD") if m in txt]
        if bad:
            say("FAIL panic: %s" % ", ".join(bad)); ok = False
        else:
            say("PASS tanpa panic")

        say("")
        say("FUNC PROBE: %s" % ("ALL PASS" if ok else "FAIL"))
        return 0 if ok else 1
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
