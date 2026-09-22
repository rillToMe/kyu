#!/usr/bin/env python3
# _menu2_probe.py — klik menu File Notepad dengan koordinat terverifikasi.
# Strategi: slam ke (0,0), verifikasi via diff screendump, maju persis ke
# tengah judul "File" (menubar window di y=112+8..112+32 → kursor di y=124).
import os, sys, time, importlib.util

spec = importlib.util.spec_from_file_location("pr", os.path.join(os.path.dirname(os.path.abspath(__file__)), "_ui_probe.py"))
pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr)

OUT = pr.OUT


def diff_top(a, b, n=5):
    w, h, pa = pr.read_ppm(a)
    _, _, pb = pr.read_ppm(b)
    cells = {}
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            i = (y * w + x) * 3
            if pa[i:i + 3] != pb[i:i + 3]:
                cells.setdefault((x // 24, y // 24), []).append((x, y))
    out = []
    for k, v in cells.items():
        if len(v) >= 4:
            xs = [p[0] for p in v]; ys = [p[1] for p in v]
            out.append((len(v), sum(xs) // len(xs), sum(ys) // len(ys)))
    out.sort(reverse=True)
    return out[:n]


def main():
    pr.prep()
    p = pr.start()
    try:
        m = pr.Mon(pr.connect())
        if not pr.wait_serial("ready", 120):
            print("boot gagal"); return
        m.type("root"); m.key("ret")
        time.sleep(0.6)
        m.type("1"); m.key("ret")
        time.sleep(3.5)
        m.type("start notepad"); m.key("ret")
        time.sleep(3.5)

        for _ in range(250): m.move(-40, -40)
        time.sleep(0.8)
        m.cmd("screendump %s/m0.ppm" % OUT, 1.2)
        # maju horizontal → kursor di (x,0)
        for _ in range(35): m.move(4, 0)
        time.sleep(0.6)
        m.cmd("screendump %s/m1.ppm" % OUT, 1.2)
        print("diff m0→m1:", diff_top("%s/m0.ppm" % OUT, "%s/m1.ppm" % OUT, 3))
        # turun ke y=124
        for _ in range(31): m.move(0, 4)
        time.sleep(0.6)
        m.cmd("screendump %s/m2.ppm" % OUT, 1.2)
        print("diff m1→m2:", diff_top("%s/m1.ppm" % OUT, "%s/m2.ppm" % OUT, 3))
        # hover dulu (panah harus menyorot judul File), lalu klik
        time.sleep(0.4)
        m.cmd("screendump %s/m3_hover.ppm" % OUT, 1.0)
        m.click()
        time.sleep(1.2)
        m.cmd("screendump %s/m4_filemenu.ppm" % OUT, 1.2)
        print("diff m3→m4:", diff_top("%s/m3_hover.ppm" % OUT, "%s/m4_filemenu.ppm" % OUT, 4))
        for name in ("m3_hover", "m4_filemenu"):
            pr.ppm_to_png("%s/%s.ppm" % (OUT, name), "%s/%s.png" % (OUT, name))
        print("--- serial tail ---")
        print(pr.serial_text()[-300:])
    finally:
        try:
            m.cmd("quit")
        except Exception:
            pass
        time.sleep(1)
        if p.poll() is None:
            p.terminate()


if __name__ == "__main__":
    main()
