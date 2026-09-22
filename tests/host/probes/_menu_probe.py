#!/usr/bin/env python3
# _menu_probe.py — cari posisi kursor lewat diff screendump, lalu klik judul menu
# Notepad (agar dropdown bisa diperiksa). Kursor PS/2 bersifat RELATIF, jadi
# posisinya harus ditemukan dulu, tidak bisa ditebak.
import os, sys, time, importlib.util

spec = importlib.util.spec_from_file_location("pr", os.path.join(os.path.dirname(os.path.abspath(__file__)), "_ui_probe.py"))
pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr)

OUT = pr.OUT


def diff_clusters(a, b):
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
        if len(v) >= 6:
            xs = [p[0] for p in v]
            ys = [p[1] for p in v]
            out.append((len(v), sum(xs) // len(xs), sum(ys) // len(ys)))
    out.sort(reverse=True)
    return out[:8]


def main():
    pr.prep()
    p = pr.start()
    try:
        m = pr.Mon(pr.connect())
        if not pr.wait_serial("ready", 120):
            print("boot gagal")
            return
        m.type("root")
        m.key("ret")
        time.sleep(0.6)
        m.type("1")
        m.key("ret")
        time.sleep(3)
        m.type("start notepad")
        m.key("ret")
        time.sleep(3)

        # Slam ke sudut kiri-atas: kursor berhenti di (0,0) karena compositor
        # membaca delta relatif dan clamp di tepi.
        for _ in range(200):
            m.move(-40, -40)
        time.sleep(0.6)
        m.cmd("screendump %s/c0.ppm" % OUT, 1.2)
        for _ in range(40):
            m.move(10, 0)
        time.sleep(0.6)
        m.cmd("screendump %s/c1.ppm" % OUT, 1.2)
        cl = diff_clusters("%s/c0.ppm" % OUT, "%s/c1.ppm" % OUT)
        print("cluster diff (n, cx, cy):", cl)

        # Dari (0,0) ke judul "File": menu x=0..720, judul pertama di x≈6..60,
        # baris menubar di y≈8..32 relatif canvas (window di x=100,y=112).
        target = (120, 124)          # koordinat layar
        # Kembali ke pojok dulu, lalu maju persis sejauh target.
        for _ in range(200):
            m.move(-40, -40)
        time.sleep(0.4)
        for _ in range(target[0] // 4):
            m.move(4, 0)
        for _ in range(target[1] // 4):
            m.move(0, 4)
        time.sleep(0.6)
        m.cmd("screendump %s/c2.ppm" % OUT, 1.2)
        cl2 = diff_clusters("%s/c1.ppm" % OUT, "%s/c2.ppm" % OUT)
        print("cluster setelah gerak (n, cx, cy):", cl2[:3])
        m.click()
        time.sleep(1.0)
        m.cmd("screendump %s/c3_filemenu.ppm" % OUT, 1.2)
        print("--- serial tail ---")
        print(pr.serial_text()[-400:])
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
