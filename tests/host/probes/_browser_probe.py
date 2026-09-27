#!/usr/bin/env python3
# _browser_probe.py — KyuzenOS Browser vertical slice di QEMU headless.
#
#   fixture serve.py (host 127.0.0.1:8771, guest 10.0.0.2:8771... via 10.0.2.2)
#   -> boot -> login -> `start browser http://10.0.2.2:8771/`
#   -> [browser/nav] -> render piksel -> klik link (serial coords) -> page2
#   -> Back/Fwd/Reload (geometri toolbar relatif blob putih) -> scroll drag
#   -> URL rusak (panel error) -> DNS gagal -> https nyata (example.com,
#   info.cern.ch via slirp) -> redirect loop -> ESC -> tanpa panic.
#
#   python tests/host/probes/_browser_probe.py
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _ui_probe as P
import _screen_text as ST

OUT = os.path.join(P.ROOT, "tests", "host", "probes", "_ui_out")
SITE = os.path.join(P.ROOT, "tests", "browser_site")
PORT = 8771


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


def dbl(m):
    m.click()
    time.sleep(0.6)
    # kursor sudah di tempat; klik kedua tanpa gerak
    m.cmd("mouse_button 1", 0.05)
    m.cmd("mouse_button 0", 0.15)
    time.sleep(0.8)


def white_blob(ppm):
    w, h, px = ST.read_ppm(ppm)

    def rgb(x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    xs, ys = [], []
    for yy in range(0, h, 3):
        for xx in range(0, w, 3):
            if rgb(xx, yy) == (255, 255, 255):
                xs.append(xx)
                ys.append(yy)
    if not xs:
        return None, (w, h, px, rgb)
    return (min(xs), min(ys), max(xs), max(ys)), (w, h, px, rgb)


def main():
    P.prep()
    fixlog = open(os.path.join(OUT, "fixture.log"), "w")
    srv = subprocess.Popen([sys.executable, os.path.join(SITE, "serve.py"), str(PORT)],
                           cwd=SITE, stdout=fixlog, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    p = P.start()
    m = None
    fails = 0

    def check(cond, name):
        nonlocal fails
        print(("PASS " if cond else "FAIL ") + name, flush=True)
        if not cond:
            fails += 1

    try:
        m = P.Mon(P.connect())
        print("monitor ok", flush=True)

        def wait_abs(needle, timeout):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if needle in P.serial_text():
                    return True
                time.sleep(0.5)
            return False

        if not P.wait_serial("ready", 180):
            print("FAIL boot")
            return 1
        print("boot ready", flush=True)
        if not wait_abs("Username", 60):
            print("FAIL username")
            return 1
        m.type("root", wait=0.25)
        m.key("ret")
        if not wait_abs("Password", 30):
            print("FAIL password")
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
        start_url = os.environ.get("BROWSER_URL", "http://10.0.2.2:8771/")
        m.type("start browser " + start_url, wait=0.2)
        time.sleep(0.5)
        m.key("ret")
        if not P.wait_serial("[browser/nav]", 120):
            print("FAIL browser load (no [browser/nav])")
            print(P.serial_text()[-800:])
            return 1
        check("[browser/nav] http://10.0.2.2:8771/ [Fixture Home]" in P.serial_text(),
              "load fixture home (serial)")
        time.sleep(2.0)
        b0 = dump(m, "b0_home")
        wb, info = white_blob(b0)
        check(wb is not None, "viewport putih terdeteksi")
        if wb is None:
            return 1
        wx0, wy0, wx1, wy1 = wb
        w, h, px, rgb = info
        print("  white blob: (%d,%d)-(%d,%d)" % (wx0, wy0, wx1, wy1), flush=True)
        # Konten: link merah #EE0000 + h1 biru #0033AA di dalam blob.
        reds = []
        for yy in range(wy0, wy1, 2):
            for xx in range(wx0, wx1, 2):
                if rgb(xx, yy) == (0xEE, 0, 0):
                    reds.append((xx, yy))
        blues = sum(1 for yy in range(wy0, wy1, 2) for xx in range(wx0, wx1, 2)
                    if rgb(xx, yy) == (0, 0x33, 0xAA))
        print("  red=%d blue=%d" % (len(reds), blues), flush=True)
        check(len(reds) > 5, "link merah dirender")
        check(blues > 20, "h1 biru dirender (CSS eksternal)")

        # Origin viewport SEJATI dari cluster merah + run serial pertama:
        # piksel merah paling kiri-atas = awal run link pertama.
        st = P.serial_text()
        links = re.findall(r"\[browser/link\] (\S+) (\d+) (\d+) (\d+) (\d+)", st)
        p2 = [x for x in links if x[0] == "/page2.html"]
        check(len(p2) > 0, "link /page2.html di-log")
        if not p2 or not reds:
            return 1
        # Pemetaan eksak (libgui: window (100,80); KWM_TITLEBAR_H=32):
        #   screen = (100 + cx, 112 + cy), cx/cy koordinat konten/viewport.
        # ox/oy di bawah hanya untuk toolbar (dihitung dari наивный mapping).
        _, lx, ly, lw, lh = p2[0]
        lx, ly, lw, lh = int(lx), int(ly), int(lw), int(lh)
        print("  run /page2: (%d,%d)+(%dx%d)" % (lx, ly, lw, lh), flush=True)
        n_nav = st.count("[browser/nav]")
        slam(m)
        goto(m, 100 + 8 + lx + lw // 2, 112 + 64 + ly + lh // 2)
        dbl(m)
        ok = False
        t0 = time.time()
        while time.time() - t0 < 60:
            if P.serial_text().count("[browser/nav]") > n_nav:
                ok = True
                break
            time.sleep(1.0)
        check(ok and "Fixture Two" in P.serial_text(), "klik link -> page2")
        time.sleep(1.5)

        # Back = tombol toolbar pertama: client (8+24, 8+14).
        n_nav = P.serial_text().count("[browser/nav]")
        slam(m)
        goto(m, 100 + 8 + 24, 112 + 8 + 14)
        dbl(m)
        ok = False
        t0 = time.time()
        while time.time() - t0 < 60:
            if P.serial_text().count("[browser/nav]") > n_nav:
                ok = True
                break
            time.sleep(1.0)
        check(ok and "Fixture Home" in P.serial_text().split("[browser/nav]")[-1],
              "Back -> home")
        time.sleep(1.5)

        # Reload = tombol ketiga: Back(~48)+6+Fwd(~40)+6 -> x=108, center +30.
        n_nav = P.serial_text().count("[browser/nav]")
        slam(m)
        goto(m, 100 + 108 + 30, 112 + 8 + 14)
        dbl(m)
        ok = False
        t0 = time.time()
        while time.time() - t0 < 60:
            if P.serial_text().count("[browser/nav]") > n_nav:
                ok = True
                break
            time.sleep(1.0)
        check(ok, "Reload muat ulang")

        # Scroll: drag scrollbar viewport (client x=108+824, y 176..646).
        #Scrollbar: x~926 tengah thumb y~300. Drag 60px ke bawah.
        b1 = dump(m, "b1_noscroll")
        slam(m)
        goto(m, 926, 300)
        m.cmd("mouse_button 1", 0.05)
        time.sleep(0.3)
        for _ in range(8):
            m.move(0, 8)
        time.sleep(0.5)
        m.cmd("mouse_button 0", 0.15)
        time.sleep(1.2)
        b2 = dump(m, "b2_scrolled")
        h1 = open(b1, "rb").read()
        h2 = open(b2, "rb").read()
        check(h1 != h2, "scroll drag ubah tampilan")
        # Validasi isi: baris merah naik (konten scroll), window tak pindah
        # (titlebar tetap y~96) — drag titlebar vs scrollbar dibedakan di sini.
        w1, h1h, px1 = ST.read_ppm(b1)
        w2, h2h, px2 = ST.read_ppm(b2)

        def redminy(pp, ww, hh):
            best = hh
            for yy in range(0, hh, 2):
                for xx in range(0, ww, 4):
                    i = (yy * ww + xx) * 3
                    if pp[i] == 0xEE and pp[i + 1] == 0 and pp[i + 2] == 0:
                        if yy < best:
                            best = yy
            return best

        def titlebar_at(pp, ww):
            # baris y=96: dominan (45,45,45) = titlebar browser
            n = 0
            for xx in range(100, 960, 4):
                i = (96 * ww + xx) * 3
                if pp[i] == 45 and pp[i + 1] == 45 and pp[i + 2] == 45:
                    n += 1
            return n

        r1 = redminy(px1, w1, h1h)
        r2 = redminy(px2, w2, h2h)
        print("  red top: %d -> %d; titlebar px: %d/%d" % (r1, r2, titlebar_at(px1, w1),
                                                           titlebar_at(px2, w2)),
              flush=True)
        check(r2 != r1, "konten pindah saat scroll (naik/keluar view)")
        check(titlebar_at(px2, w2) > 100, "window tak pindah")

        # URL rusak: klik address bar (tombol lebih lebar dari kiraan;
        # masuk jauh ke dalam box: client x~350), End, hapus, port mati, Enter.
        def goto_url(url):
            slam(m)
            goto(m, 100 + 350, 112 + 8 + 14)
            dbl(m)
            time.sleep(0.8)
            m.key("end")
            time.sleep(0.3)
            for _ in range(60):
                m.key("backspace", wait=0.03)
            time.sleep(0.5)
            m.type(url, wait=0.15)
            time.sleep(0.3)
            m.key("ret")

        goto_url("http://10.0.2.2:9/")
        ok = P.wait_serial("[browser/error]", 60)
        check(ok, "URL mati -> panel error")
        check("connection failed" in P.serial_text(), "alasan conn ditampilkan")
        time.sleep(1.0)
        b3 = dump(m, "b3_error")

        # Failure matrix (§78): DNS, https, redirect-loop — via address bar
        # (panel error tidak menutup toolbar, jadi navigasi berikutnya jalan).
        goto_url("http://x.invalid/")
        ok = P.wait_serial("DNS lookup failed", 60)
        check(ok, "DNS gagal -> error jujur")

        # HTTPS nyata (TLS 1.2 ECDHE+AES-128-GCM, X.509 penuh via slirp).
        # Fixture TLS lokal tak bisa dipakai tamu (BearSSL hanya cocokkan
        # SAN DNS; IP literal tak tervalidasi) — situs nyata sebagai gantinya.
        goto_url("https://example.com/")
        ok = P.wait_serial("[browser/nav] https://example.com/", 120)
        check(ok, "https example.com -> halaman dimuat")
        check("Example Domain" in P.serial_text(), "judul example.com tampil")
        time.sleep(1.0)

        goto_url("https://info.cern.ch/")
        ok = P.wait_serial("[browser/nav] https://info.cern.ch/", 150)
        check(ok, "https info.cern.ch -> halaman dimuat")

        # Regresi parse_px (google.com 91KB: value CSS 1-2 char underflow
        # size()-2 -> npos -> #UD di realloc). Halaman besar harus dimuat,
        # bukan BSOD.
        goto_url("http://www.google.com/")
        ok = P.wait_serial("[browser/nav] http://www.google.com/", 180)
        check(ok, "http google.com -> halaman besar dimuat")
        check("Google" in P.serial_text(), "judul Google tampil")

        goto_url("http://10.0.2.2:8771/loop1")
        ok = P.wait_serial("too many redirects", 90)
        check(ok, "redirect loop -> dibatasi")

        m.key("esc")
        time.sleep(1.5)
        st = P.serial_text()
        check("[PANIC] handler masuk" not in st, "tanpa panic")
        print("fails=%d" % fails, flush=True)
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
        try:
            srv.terminate()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
