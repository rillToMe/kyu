#!/usr/bin/env python3
# _layout_probe.py — verifikasi RUNTIME Phase D di QEMU headless.
#
# Memakai harness tests/host/probes/_ui_probe.py (boot -> login root/1 ->
# `start widget_demo`) + panel "Baru" demo (Radio/ComboBox/Separator/Grid/
# Tooltip). Deteksi ADAPTIF: posisi widget ditemukan dari piksel (dot aksen,
# kotak combo), bukan koordinat absolut — tahan terhadap пропис window.
#
#   Tab -> strip fokus -> Right x4 -> panel Baru (tab index 4)
#   klik radio "Siang" (di bawah dot terdeteksi) -> dot pindah +28y
#   Down -> radio "Malam" (+28y)
#   Tab -> ComboBox -> Enter (popup) -> Down+Enter (commit) -> Enter+Esc
#   hover tombol tooltip -> tooltip muncul
#   statis: separator + grid 2x2
#
# Warna legacy demo: bg 0x121212, accent 0xE94560, panel 0x252526,
# elevated 0x2A4A7E, surface 0x0F3460, subtle 0x333333. Hanya fill solid
# yang dihitung (teks anti-aliased compositor — lihat _fileman_probe.py).
#
#   python tests/host/probes/_layout_probe.py
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _ui_probe as P
import _screen_text as ST

OUT = os.path.join(P.ROOT, "tests", "host", "probes", "_ui_out")
BG = (0x12, 0x12, 0x12)
ACC = (0xE9, 0x45, 0x60)
PANEL = (0x25, 0x25, 0x26)
ELEV = (0x2A, 0x4A, 0x7E)
SURF = (0x0F, 0x34, 0x60)
SUBTLE = (0x33, 0x33, 0x33)
PITCH = 28   # radio: h20 + spacing8


def dump(m, name, wait=1.0):
    ppm = os.path.join(OUT, name + ".ppm")
    m.cmd("screendump %s" % ppm, wait)
    return ppm


def blobs(ppm, x0, y0, x1, y1, color, min_h=0):
    """Daftar cluster piksel `color` (konektivitas 8-arah sederhana).
    min_h = tinggi minimum cluster (saring strip underline 2px vs dot)."""
    w, h, px = ST.read_ppm(ppm)
    x0 = max(0, x0); y0 = max(0, y0)
    x1 = min(w - 1, x1); y1 = min(h - 1, y1)
    pts = set()
    for y in range(y0, y1 + 1):
        base = y * w * 3
        for x in range(x0, x1 + 1):
            i = base + x * 3
            if px[i] == color[0] and px[i + 1] == color[1] and px[i + 2] == color[2]:
                pts.add((x, y))
    out = []
    while pts:
        seed = pts.pop()
        stack = [seed]
        members = []
        while stack:
            p = stack.pop()
            members.append(p)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    q = (p[0] + dx, p[1] + dy)
                    if q in pts:
                        pts.discard(q)
                        stack.append(q)
        out.append(members)
    if min_h > 0:
        out = [c for c in out
               if max(p[1] for p in c) - min(p[1] for p in c) + 1 >= min_h]
    return out


def centroid(members):
    n = len(members)
    return (sum(p[0] for p in members) / n, sum(p[1] for p in members) / n)


def window_bbox(ppm, step=2):
    w, h, px = ST.read_ppm(ppm)
    xs, ys = [], []
    for y in range(0, h, step):
        for x in range(0, w, step):
            i = (y * w + x) * 3
            if (px[i], px[i + 1], px[i + 2]) == BG:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs), max(ys))


def slam(m):
    for _ in range(250):
        m.move(-40, -40)
    time.sleep(0.4)


def goto(m, x, y):
    cx, cy = 0, 0
    while cx < x:
        d = min(8, x - cx)
        m.move(d, 0)
        cx += d
    while cy < y:
        d = min(8, y - cy)
        m.move(0, d)
        cy += d
    time.sleep(0.4)


def main():
    P.prep()
    p = P.start()
    m = None
    fails = 0

    def check(cond, name):
        nonlocal fails
        print(("PASS " if cond else "FAIL ") + name)
        if not cond:
            fails += 1

    try:
        m = P.Mon(P.connect())
        print("monitor ok")
        if not P.wait_serial("ready", 120):
            print("FAIL boot")
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
        m.type("start widget_demo")
        m.key("ret")
        time.sleep(3.0)

        base = dump(m, "l0_base")
        bb = window_bbox(base)
        check(bb is not None, "demo window tergambar")
        if bb is None:
            return 1
        wx, wy, wx1, wy1 = bb
        print("demo bbox: (%d,%d)-(%d,%d)" % (wx, wy, wx1, wy1))

        # Panel "Baru" = tab index 4.
        m.key("tab")
        time.sleep(0.8)
        for _ in range(4):
            m.key("right")
            time.sleep(0.25)
        time.sleep(0.8)
        b1 = dump(m, "l1_baru")

        # Dot "Pagi": satu cluster aksen kecil di panel (saring underline
        # strip tab yang pipih 2px via min_h).
        dots = [c for c in blobs(b1, wx, wy, wx1, wy1, ACC, min_h=5)
                if len(c) < 200]
        check(len(dots) >= 1, "radio dot awal ada (Pagi)")
        if not dots:
            return 1
        dx0, dy0 = centroid(max(dots, key=len))
        print("  dot0 @ (%.1f, %.1f)" % (dx0, dy0))
        dx2, dy2 = dx0, dy0   # fallback separator bila langkah panah gagal

        # Klik "Siang": tepat di bawah dot (pitch 28).
        slam(m)
        goto(m, int(dx0), int(dy0) + PITCH)
        m.click()
        time.sleep(1.0)
        b2 = dump(m, "l2_siang")
        dots = [c for c in blobs(b2, wx, wy, wx1, wy1, ACC, min_h=5)
                if len(c) < 200]
        ok = False
        if dots:
            dx1, dy1 = centroid(max(dots, key=len))
            print("  dot1 @ (%.1f, %.1f)" % (dx1, dy1))
            ok = abs(dx1 - dx0) < 10 and abs((dy1 - dy0) - PITCH) < 8
        check(ok, "radio klik pindah (Siang)")

        # Down -> "Malam".
        m.key("down")
        time.sleep(0.8)
        b3 = dump(m, "l3_malam")
        dots = [c for c in blobs(b3, wx, wy, wx1, wy1, ACC, min_h=5)
                if len(c) < 200]
        ok = False
        if dots:
            dx2, dy2 = centroid(max(dots, key=len))
            print("  dot2 @ (%.1f, %.1f)" % (dx2, dy2))
            ok = abs(dx2 - dx0) < 10 and abs((dy2 - dy0) - 2 * PITCH) < 8
        check(ok, "radio panah pindah (Malam)")

        # Tab -> ComboBox: border fokus aksen di sekitar kotaknya.
        # Kotak combo = blob SURF (surface) terbesar di panel.
        m.key("tab")
        time.sleep(0.8)
        b4 = dump(m, "l4_combo_focus")
        surfs = blobs(b4, wx, wy, wx1, wy1, SURF)
        ok = False
        cb = None
        if surfs:
            cb = max(surfs, key=len)
            xs = [pp[0] for pp in cb]
            ys = [pp[1] for pp in cb]
            cb = (min(xs), min(ys), max(xs), max(ys))
            cx0, cy0 = (cb[0] + cb[2]) / 2, cb[1] - 1
            acc, _, _ = (0, 0, 0)
            # border atas kotak: baris tepat di atasnya harus aksen (fokus)
            w, h, px = ST.read_ppm(b4)
            n = 0
            for x in range(cb[0], cb[2] + 1):
                i = (int(cy0) * w + x) * 3
                if (px[i], px[i + 1], px[i + 2]) == ACC:
                    n += 1
            print("  combo box: %s border-aksen: %d" % (cb, n))
            ok = n > 40
        check(ok, "combo fokus (border aksen)")

        # Enter -> popup panel di bawah box.
        m.key("ret")
        time.sleep(0.8)
        b5 = dump(m, "l5_popup")
        pans = blobs(b5, wx, wy, wx1, wy1, PANEL)
        ok = any(len(c) > 2000 for c in pans)
        print("  popup panel px: %d" % max([len(c) for c in pans] or [0]))
        check(ok, "combo popup terbuka")

        # Down + Enter -> commit + tutup.
        m.key("down")
        time.sleep(0.5)
        m.key("ret")
        time.sleep(0.8)
        b6 = dump(m, "l6_commit")
        pans = blobs(b6, wx, wy, wx1, wy1, PANEL)
        check(not any(len(c) > 500 for c in pans), "combo commit menutup popup")

        # Enter (buka) + Esc (batal).
        m.key("ret")
        time.sleep(0.8)
        m.key("esc")
        time.sleep(0.8)
        b7 = dump(m, "l7_esc")
        pans = blobs(b7, wx, wy, wx1, wy1, PANEL)
        check(not any(len(c) > 500 for c in pans), "combo esc menutup popup")

        # Tooltip: hover tombol di bawah grid (ELEV terbawah), tunggu > 600ms.
        # Metode delta: piksel ELEV BARU di atas tombol vs dump pra-hover.
        elevs = blobs(b7, wx, wy, wx1, wy1, ELEV)
        ok = False
        if elevs:
            lows = sorted(elevs, key=lambda c: centroid(c)[1])
            tx, ty = centroid(lows[-1])
            print("  hover target @ (%.1f, %.1f)" % (tx, ty))
            slam(m)
            goto(m, int(tx), int(ty))
            time.sleep(1.6)
            b8 = dump(m, "l8_tip")
            w8, h8, px8 = ST.read_ppm(b8)
            _, _, px7 = ST.read_ppm(b7)
            fresh = 0
            for y in range(max(0, int(ty) - 60), max(0, int(ty) - 8)):
                for x in range(max(0, int(tx) - 100), min(w8, int(tx) + 100)):
                    i = (y * w8 + x) * 3
                    if (px8[i], px8[i + 1], px8[i + 2]) == ELEV:
                        j = (y * w8 + x) * 3
                        if not (px7[j] == ELEV[0] and px7[j + 1] == ELEV[1] and
                                px7[j + 2] == ELEV[2]):
                            fresh += 1
            print("  tooltip piksel baru: %d" % fresh)
            ok = fresh > 150
        check(ok, "tooltip muncul saat hover")

        # Statis: separator (run SUBTLE panjang, di bawah radio).
        # Langkah 1px (garis 1px lolos dari langkah 2 bila paritas meleset).
        w, h, px = ST.read_ppm(b7)
        best = 0
        for y in range(int(dy2) + 10, wy1 + 1):
            run = 0
            for x in range(wx, wx1 + 1):
                i = (y * w + x) * 3
                if (px[i], px[i + 1], px[i + 2]) == SUBTLE:
                    run += 1
                    best = max(best, run)
                else:
                    run = 0
            if best > 150:
                break
        check(best > 150, "separator horizontal ada")
        print("  sep run: %d" % best)
        big = [c for c in blobs(b7, wx, wy, wx1, wy1, ELEV) if len(c) > 800]
        check(len(big) >= 3, "grid 2x2 tombol ada")
        print("  grid blobs: %d" % len(big))

        tail = P.serial_text()
        if "BSOD" in tail or "handler masuk" in tail or "#PF(" in tail:
            print("FAIL serial: jejak panic")
            fails += 1
        else:
            print("PASS serial bersih")

        for p_ in sorted(os.listdir(OUT)):
            if p_.endswith(".ppm") and p_.startswith("l"):
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
    print("LAYOUT-PROBE: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
