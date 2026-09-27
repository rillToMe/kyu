#!/usr/bin/env python3
# _focus_probe.py — verifikasi RUNTIME traversal fokus Phase C di QEMU headless.
#
# Memakai harness tests/host/probes/_ui_probe.py (boot -> login root/1 ->
# `start widget_demo`) lalu membuktikan lewat screendump PPM:
#
#   1. baseline: tanpa fokus keyboard (tak ada ring fokus aksen di kontrol).
#   2. Tab 1x: fokus masuk ke stop pertama (strip Tab demo) -> ring aksen muncul.
#   3. Tab 2x: fokus pindah ke tombol "+1" -> centroid ring BERGESER.
#   4. Shift+Tab: fokus kembali ke strip -> centroid kembali ke posisi (2).
#   5. Serial bersih (tanpa panic/BSOD) selama sequence.
#
# Demo memakai tema legacy (aksen 0xE9,0x45,0x60): ring fokus = border yang
# berubah menjadi accent. Metrik = piksel aksen, bukan teks (teks toolkit
# anti-aliased oleh compositor — lihat _fileman_probe.py).
#
#   python tests/host/probes/_focus_probe.py
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _ui_probe as P
import _screen_text as ST

OUT = os.path.join(P.ROOT, "tests", "host", "probes", "_ui_out")
ACCENT = (0xE9, 0x45, 0x60)   # demo legacy accent = theme.focus
DELTA_MIN = 40                # ring fokus minimal ~2*(w+h) px
MOVE_MIN = 15.0               # centroid harus bergeser (px) antar stop


def dump(m, name, wait=1.0):
    ppm = os.path.join(OUT, name + ".ppm")
    m.cmd("screendump %s" % ppm, wait)
    return ppm


def accent_stats(ppm):
    """(count, cx, cy) piksel aksen."""
    w, h, px = ST.read_ppm(ppm)
    n = sx = sy = 0
    for y in range(h):
        base = y * w * 3
        for x in range(w):
            i = base + x * 3
            if px[i] == ACCENT[0] and px[i + 1] == ACCENT[1] and px[i + 2] == ACCENT[2]:
                n += 1
                sx += x
                sy += y
    if n == 0:
        return (0, 0.0, 0.0)
    return (n, sx / n, sy / n)


def main():
    P.prep()
    p = P.start()
    m = None
    fails = 0
    try:
        m = P.Mon(P.connect())
        print("monitor ok")
        if not P.wait_serial("ready", 120):
            print("FAIL boot: serial belum 'ready'")
            return 1
        print("boot ready")
        m.type("root")
        m.key("ret")
        time.sleep(0.6)
        m.type("1")
        m.key("ret")
        if not P.wait_serial("root@kyuzen>", 40):
            print("FAIL login")
            return 1
        time.sleep(1.0)
        print("logged in")
        m.type("start widget_demo")
        m.key("ret")
        time.sleep(3.0)

        base = dump(m, "f0_base")
        n0, _, _ = accent_stats(base)
        print("baseline accent px: %d" % n0)

        m.key("tab")
        time.sleep(1.0)
        t1 = dump(m, "f1_tab1")
        n1, x1, y1 = accent_stats(t1)
        print("tab1 accent px: %d @ (%.1f, %.1f)" % (n1, x1, y1))
        if n1 - n0 < DELTA_MIN:
            print("FAIL tab1: ring fokus tak muncul (delta=%d)" % (n1 - n0))
            fails += 1
        else:
            print("PASS tab1: ring fokus muncul")

        m.key("tab")
        time.sleep(1.0)
        t2 = dump(m, "f2_tab2")
        n2, x2, y2 = accent_stats(t2)
        print("tab2 accent px: %d @ (%.1f, %.1f)" % (n2, x2, y2))
        dist = ((x2 - x1) ** 2 + (y2 - y1) ** 2) ** 0.5
        if dist < MOVE_MIN:
            print("FAIL tab2: ring tak berpindah (dist=%.1f)" % dist)
            fails += 1
        else:
            print("PASS tab2: fokus pindah (dist=%.1f)" % dist)

        m.key("shift-tab")
        time.sleep(1.0)
        t3 = dump(m, "f3_stab")
        n3, x3, y3 = accent_stats(t3)
        print("stab accent px: %d @ (%.1f, %.1f)" % (n3, x3, y3))
        back = ((x3 - x1) ** 2 + (y3 - y1) ** 2) ** 0.5
        if abs(n3 - n1) > DELTA_MIN and back >= MOVE_MIN:
            print("FAIL stab: tak kembali ke stop tab1")
            fails += 1
        else:
            print("PASS stab: kembali ke stop sebelumnya")

        tail = P.serial_text()
        # Indikator panic nyata (bukan "[PANIC_LOG]" boot normal): BSOD /
        # handler panik / fault report.
        if "BSOD" in tail or "handler masuk" in tail or "#PF(" in tail:
            print("FAIL serial: jejak panic")
            fails += 1
        else:
            print("PASS serial bersih")

        for p_ in sorted(os.listdir(OUT)):
            if p_.endswith(".ppm") and p_.startswith("f"):
                try:
                    P.ppm_to_png(os.path.join(OUT, p_),
                                 os.path.join(OUT, p_[:-4] + ".png"))
                except Exception:
                    pass
    finally:
        try:
            if m is not None:
                m.cmd("quit")
        except Exception:
            pass
        time.sleep(1)
        if p.poll() is None:
            p.terminate()
    print("FOCUS-PROBE: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
