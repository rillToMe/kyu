#!/usr/bin/env python3
# _xml_probe.py — verifikasi RUNTIME Phase E di QEMU headless.
#
# Memakai harness _ui_probe.py (boot -> login root/1 -> `start xml_demo`).
# xml_demo = SELURUH UI dari satu string XML (dark+purple) + binding C.
# Deteksi ADAPTIF (blob warna), bukan koordinat absolut.
#
#   python tests/host/probes/_xml_probe.py
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _ui_probe as P
import _screen_text as ST

OUT = os.path.join(P.ROOT, "tests", "host", "probes", "_ui_out")
BG = (0x11, 0x11, 0x11)       # dark bg (theme-config)
SURF = (0x18, 0x18, 0x18)     # dark surface
ACC = (0xA3, 0x71, 0xF7)      # purple accent
PITCH = 28                    # radio h20 + spacing sm 8


def dump(m, name, wait=1.0):
    ppm = os.path.join(OUT, name + ".ppm")
    m.cmd("screendump %s" % ppm, wait)
    return ppm


def blobs(ppm, x0, y0, x1, y1, color, min_h=0):
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
    bb = (min(xs), min(ys), max(xs), max(ys))
    # Artefak desktop (bayangan/taskbar) bisa menyisakan segelintir piksel bg:
    # window nyata = persegi besar.
    if bb[2] - bb[0] < 100 or bb[3] - bb[1] < 100:
        return None
    return bb


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
        m.type("start xml_demo")
        m.key("ret")
        time.sleep(3.0)

        base = dump(m, "x0_base")
        bb = window_bbox(base)
        check(bb is not None, "xml window tergambar (dark bg)")
        if bb is None:
            return 1
        wx, wy, wx1, wy1 = bb
        print("xml bbox: (%d,%d)-(%d,%d)" % (wx, wy, wx1, wy1))

        # Kalibrasi offset: Apply primer = blob aksen TERBESAR (lokal 8,209,
        # 64x28 — deterministik dari host, lihat GEO di libui_xml_test).
        # offset = posisi layar - posisi lokal; semua target diturunkan darinya
        # (tahan terhadap frame/titlebar KWM).
        acc = blobs(base, wx, wy, wx1, wy1, ACC, min_h=5)
        big = [c for c in acc if len(c) > 500]
        check(len(big) >= 1, "tombol Apply primer (aksen)")
        if not big:
            return 1
        ab = max(big, key=len)
        ax0 = min(p[0] for p in ab)
        ay0 = min(p[1] for p in ab)
        ox, oy = ax0 - 8, ay0 - 209
        print("  apply @ (%d,%d) offset=(%d,%d)" % (ax0, ay0, ox, oy))
        apply_c = centroid(ab)

        # Dot radio m1: cluster aksen kecil di band radio (lokal y 96..116).
        dots = [c for c in acc
                if len(c) < 80
                and oy + 96 <= sum(p[1] for p in c) / len(c) <= oy + 116]
        check(len(dots) >= 1, "radio dot awal (Basic)")
        if not dots:
            return 1
        dx0, dy0 = centroid(max(dots, key=len))
        print("  dot0 @ (%.1f, %.1f)" % (dx0, dy0))

        # Klik lingkaran m2 (lokal 132,96: tengah lingkaran = +6,+6).
        slam(m)
        goto(m, ox + 138, oy + 102)
        m.click()
        time.sleep(1.0)
        # Klik kedua (jaminan: klik pertama mungkin hanya fokus window).
        goto(m, ox + 138, oy + 102)
        m.click()
        time.sleep(1.0)
        b2 = dump(m, "x1_adv")
        acc2 = blobs(b2, wx, wy, wx1, wy1, ACC, min_h=5)
        dots = [c for c in acc2
                if len(c) < 80
                and oy + 96 <= sum(p[1] for p in c) / len(c) <= oy + 116]
        ok = False
        if dots:
            dx1, dy1 = centroid(max(dots, key=len))
            print("  dot1 @ (%.1f, %.1f)" % (dx1, dy1))
            ok = abs((dx1 - dx0) - 68) < 12 and abs(dy1 - dy0) < 8
        check(ok, "radio klik pindah (Advanced)")

        # Reset fokus: klik latar kosong pojok window, lalu Tab sekali ->
        # stop fokus pertama = ComboBox (DFS: label dilewati).
        slam(m)
        goto(m, wx1 - 30, wy1 - 30)
        m.click()
        time.sleep(0.6)
        m.key("tab")
        time.sleep(0.8)
        b3 = dump(m, "x2_combo_focus")
        surfs = blobs(b3, wx, wy, wx1, wy1, SURF)
        ok = False
        if surfs:
            # Kotak combo 140x24 (textbox 200x24 dikecualikan via lebar).
            cands = []
            for c in surfs:
                xs = [pp[0] for pp in c]
                ys = [pp[1] for pp in c]
                cw, ch = max(xs) - min(xs), max(ys) - min(ys)
                if 100 <= cw <= 170 and 15 <= ch <= 30:
                    cands.append((c, min(xs), min(ys), max(xs), max(ys)))
            if cands:
                c, x0, y0, x1, y1 = max(cands, key=lambda t: len(t[0]))
                w, h, px = ST.read_ppm(b3)
                n = 0
                for x in range(x0, x1 + 1):
                    i = (max(0, y0 - 1) * w + x) * 3
                    if (px[i], px[i + 1], px[i + 2]) == ACC:
                        n += 1
                print("  combo box: (%d,%d)-(%d,%d) border-aksen: %d" % (x0, y0, x1, y1, n))
                ok = n > 40
        check(ok, "tab traversal -> combo fokus")

        # Enter -> popup; Esc -> tutup.
        m.key("ret")
        time.sleep(0.8)
        b4 = dump(m, "x3_popup")
        w, h, px = ST.read_ppm(base)
        _, _, px4 = ST.read_ppm(b4)
        diff = sum(1 for i in range(0, len(px), 3 * 7)
                   if px[i:i + 3] != px4[i:i + 3])
        print("  delta popup: %d" % diff)
        check(diff > 300, "combo popup terbuka")
        m.key("esc")
        time.sleep(0.8)
        b5 = dump(m, "x4_no_popup")
        _, _, px5 = ST.read_ppm(b5)
        diff2 = sum(1 for i in range(0, len(px), 3 * 7)
                    if px[i:i + 3] != px5[i:i + 3])
        print("  delta pasca-esc: %d" % diff2)
        check(diff2 < diff / 2, "esc tutup popup (native)")

        # Slider via keyboard: band slider = lokal (8,177,160x20) + offset.
        # Handle = blob aksen kecil di band. Tab x5 dari combo
        # (checkbox, m1, m2, textbox, slider).
        sx0, sy0, sx1, sy1 = ox + 8, oy + 177, ox + 168, oy + 197
        for _ in range(5):
            m.key("tab")
            time.sleep(0.3)
        time.sleep(0.6)
        # Handle = satu-satunya yang aksen SETINGGI slider (20px); outline
        # track fokus hanya 6px. Kolom dengan >=16 px aksen = kolom handle.
        def handle_x(ppm):
            w, h, px = ST.read_ppm(ppm)
            xs = []
            for x in range(sx0 + 4, sx1 - 3):
                n = 0
                for y in range(sy0, sy1 + 1):
                    i = (y * w + x) * 3
                    if (px[i], px[i + 1], px[i + 2]) == ACC:
                        n += 1
                if n >= 16:
                    xs.append(x)
            if not xs:
                return (0, 0)
            return (sum(xs) / len(xs), len(xs))
        bs = dump(m, "x5_slider0")
        h0, n0 = handle_x(bs)
        for _ in range(3):
            m.key("right")
            time.sleep(0.3)
        time.sleep(0.6)
        bs2 = dump(m, "x6_slider1")
        h1, n1 = handle_x(bs2)
        print("  handle %.1f -> %.1f (n=%d/%d)" % (h0, h1, n0, n1))
        check(n0 >= 4 and n1 >= 4 and (h1 - h0) > 20, "slider keyboard geser kanan")

        # Tooltip: hover textbox (lokal 8,141,200x24) -> blob baru.
        tx0, ty0, tx1, ty1 = ox + 8, oy + 141, ox + 208, oy + 165
        slam(m)
        goto(m, (tx0 + tx1) // 2, (ty0 + ty1) // 2)
        time.sleep(2.0)
        bt = dump(m, "x7_tooltip")
        _, _, pxt = ST.read_ppm(bt)
        ddiff = sum(1 for i in range(0, len(px5), 3 * 11)
                    if px5[i:i + 3] != pxt[i:i + 3])
        print("  delta tooltip: %d" % ddiff)
        check(ddiff > 150, "tooltip hover muncul")

        # Cancel (binding CLICK): tengah Cancel lokal (80,209,72x28).
        slam(m)
        goto(m, ox + 116, oy + 223)
        m.click()
        time.sleep(1.0)
        goto(m, ox + 116, oy + 223)
        m.click()
        time.sleep(1.5)
        bc = dump(m, "x8_closed")
        check(window_bbox(bc) is None, "cancel tutup window (binding)")

        st = P.serial_text()
        check("[PANIC] handler masuk" not in st, "tanpa panic")
        print("fails=%d" % fails)
        return 1 if fails else 0
    finally:
        try:
            if m is not None:
                m.cmd("quit")
        except Exception:
            pass
        try:
            p.terminate()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
