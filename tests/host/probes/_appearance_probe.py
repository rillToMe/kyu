#!/usr/bin/env python3
# _appearance_probe.py — Phase F: AppearancePage XML di QEMU headless.
#
#   boot -> login -> `start settings` -> klik baris Appearance di sidebar
#   (serial "[settings] Appearance") -> klik tombol Dark -> status berubah
#   (delta piksel) -> ESC -> tanpa panic.
#
# Geometri diturunkan dari screenshot (sidebar 150px, baris 20px,
# Appearance = baris 2; form XML: 2 label + 3 tombol + status), bukan
# koordinat absolut. Warna independen-tema (deteksi perubahan, bukan nilai).
#
#   python tests/host/probes/_appearance_probe.py
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _ui_probe as P
import _screen_text as ST

OUT = os.path.join(P.ROOT, "tests", "host", "probes", "_ui_out")
THEME_BGS = [(0x12, 0x12, 0x12), (0xF0, 0xF0, 0xF0),
             (0x0D, 0x1F, 0x14), (0x1A, 0x1A, 0x2E)]


def dump(m, name, wait=1.0):
    ppm = os.path.join(OUT, name + ".ppm")
    m.cmd("screendump %s" % ppm, wait)
    return ppm


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

        # Prompt cepat (ret -> Password dalam ms) harus dicari ABSOLUT:
        # anchor-pos sesudah kirim-tombol bisa mendarat SETELAH respons
        # tercetak (= miss). Trace event (Appearance/dll) tetap pos-anchored.
        def wait_abs(needle, timeout):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if needle in P.serial_text():
                    return True
                time.sleep(0.5)
            return False

        def wait_new(needle, timeout):
            pos = len(P.serial_text())
            t0 = time.time()
            while time.time() - t0 < timeout:
                if needle in P.serial_text()[pos:]:
                    return True
                time.sleep(0.5)
            return False

        # Ketik berbasis prompt (ketikan dini hilang saat decode font/wallpaper).
        if not P.wait_serial("ready", 180):
            print("FAIL boot")
            return 1
        print("boot ready")
        if not wait_abs("Username", 60):
            print("FAIL username")
            print(P.serial_text()[-400:])
            return 1
        m.type("root", wait=0.25)
        m.key("ret")
        if not wait_abs("Password", 30):
            print("FAIL password")
            print(P.serial_text()[-400:])
            return 1
        time.sleep(0.5)
        m.type("1", wait=0.25)
        m.key("ret")
        if not wait_abs("kyuzen>", 90):
            print("FAIL login")
            return 1
        t0 = time.time()
        while time.time() - t0 < 180:
            if P.wait_serial("[desktop] shell started", 20):
                break
            time.sleep(1.0)
        else:
            print("FAIL desktop")
            return 1
        time.sleep(2.0)
        m.type("start settings", wait=0.25)
        time.sleep(0.5)
        m.key("ret")
        if not P.wait_serial("[settings] start", 60):
            print("FAIL settings start")
            return 1
        time.sleep(2.0)
        base = dump(m, "a0_base")

        # Window 720px: cari warna bg dengan bentang terlebar ~720.
        w, h, px = ST.read_ppm(base)

        def rgb(x, y):
            i = (y * w + x) * 3
            return (px[i], px[i + 1], px[i + 2])

        win = None
        for color in THEME_BGS:
            xs, ys = [], []
            for yy in range(0, h, 2):
                for xx in range(0, w, 2):
                    if rgb(xx, yy) == color:
                        xs.append(xx)
                        ys.append(yy)
            if xs and 650 <= max(xs) - min(xs) <= 790:
                win = (min(xs), min(ys), max(xs), max(ys))
                break
        check(win is not None, "window settings 720px")
        if win is None:
            return 1
        wx, wy, wx1, wy1 = win
        print("settings bbox: (%d,%d)-(%d,%d)" % (wx, wy, wx1, wy1))

        # Sidebar: panel kiri 150px. Baris terpilih (System) = satu-satunya
        # pita 20px yang beda dari warna dominan panel. Appearance = +2 baris.
        sbx0, sbx1 = wx + 8, wx + 8 + 150
        from collections import Counter
        dom = Counter()
        for yy in range(wy, wy1, 4):
            for xx in range(sbx0, sbx1, 4):
                dom[rgb(xx, yy)] += 1
        listbg = dom.most_common(1)[0][0]
        sbx = (sbx0 + sbx1) // 2
        bands = []
        y = wy
        while y < wy1:
            if rgb(sbx, y) != listbg:
                y0 = y
                while y < wy1 and rgb(sbx, y) != listbg:
                    y += 2
                if 16 <= y - y0 <= 24:
                    bands.append((y0, y))
            else:
                y += 2
        check(len(bands) >= 1, "baris sidebar terpilih terdeteksi")
        if not bands:
            return 1
        # Via keyboard dulu ke Personalization (andal), lalu +20px = Appearance
        # (pola _settings_probe.py: sel_y + 20).
        n_pers = P.serial_text().count("[settings] Personalization")
        got = False
        for _ in range(3):
            m.key("n")
            time.sleep(1.0)
            if P.serial_text().count("[settings] Personalization") > n_pers:
                got = True
                break
        check(got, "ke Personalization via keyboard")
        if not got:
            return 1
        b0 = dump(m, "a0_pers")
        w0, h0, px0 = ST.read_ppm(b0)

        def rgb0(x, y):
            i = (y * w0 + x) * 3
            return (px0[i], px0[i + 1], px0[i + 2])

        # Highlight terpilih: pita non-bg ≥100px di x100-260, tinggi 12..28.
        bg0 = rgb0(150, 620) if h0 > 620 else rgb0(150, h0 - 10)
        sel_y = None
        yy = 120
        while yy < 320:
            run = 0
            for xx in range(100, 260):
                if rgb0(xx, yy) != bg0:
                    run += 1
                else:
                    if run >= 100:
                        break
                    run = 0
            if run >= 100:
                y0 = yy
                while yy < 320:
                    run2 = sum(1 for xx in range(100, 260) if rgb0(xx, yy) != bg0)
                    if run2 < 100:
                        break
                    yy += 2
                if 12 <= yy - y0 <= 28:
                    sel_y = (y0 + yy) // 2
                    break
            yy += 2
        check(sel_y is not None, "highlight Personalization ditemukan")
        if sel_y is None:
            return 1
        print("  sel_y=%d appearance y=%d" % (sel_y, sel_y + 20))
        r2y = sel_y + 20

        # Klik baris Appearance (2x: fokus + aksi) -> serial.
        slam(m)
        goto(m, sbx, r2y)
        m.click()
        time.sleep(0.8)
        goto(m, sbx, r2y)
        m.click()
        time.sleep(1.0)
        ok = P.wait_serial("[settings] Appearance", 15)
        check(ok, "navigasi Appearance (serial)")
        b1 = dump(m, "a1_appearance")

        # Form XML: page_top = atas list (satu hbox dengan sidebar);
        # Dark = tombol pertama (2 label 16px + spacing 6) -> tengah +58.
        page_x = wx + 8 + 150 + 8
        top = sel_y - 30
        dark_x, dark_y = page_x + 28, top + 58
        # Status: di bawah 3 tombol (3x28 + 2x6) + jeda.
        st_y0 = top + 44 + 3 * 28 + 2 * 6 + 4
        w1, h1, px1 = ST.read_ppm(b1)

        def region(pxa, ww, x0, y0, x1, y1):
            n = 0
            for yy in range(max(0, y0), min(h1, y1), 2):
                for xx in range(max(0, x0), min(w1, x1), 2):
                    i = (yy * ww + xx) * 3
                    n += pxa[i] + pxa[i + 1] * 3 + pxa[i + 2] * 7
            return n

        before = region(px1, w1, page_x, st_y0, page_x + 300, st_y0 + 24)
        slam(m)
        goto(m, dark_x, dark_y)
        m.click()
        time.sleep(0.8)
        goto(m, dark_x, dark_y)
        m.click()
        time.sleep(1.2)
        b2 = dump(m, "a2_dark")
        w2, h2, px2 = ST.read_ppm(b2)
        after = region(px2, w2, page_x, st_y0, page_x + 300, st_y0 + 24)
        print("  status hash: %d -> %d" % (before, after))
        check(before != after, "klik Dark ubah status (XML binding)")

        m.key("esc")
        time.sleep(1.5)
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
