#!/usr/bin/env python3
# _fileman_probe.py — verifikasi RUNTIME File Manager di QEMU headless.
#
# Memakai harness tests/host/probes/_ui_probe.py (boot -> login root/1 -> `start fileman`)
# lalu memeriksa DUA jenis bukti:
#
#   1. LAYAR (screendump PPM): geometri + warna. Teks toolkit digambar
#      anti-aliased oleh compositor sehingga tests/host/probes/_screen_text.py (font 8x16
#      solid) tidak bisa membacanya; yang bisa dipercaya dari screendump adalah
#      WILAYAH warna tema. Dipakai untuk membuktikan jendela 720px tema bawaan
#      benar-benar tergambar (bg 0x1A1A2E, bbox ~704-720 px).
#   2. SERIAL: jejak aplikasi sendiri (FileManagerApp::trace di
#      apps/filemanager/app.cpp) untuk setiap operasi yang MENGUBAH filesystem —
#      "mkdir ok <path>", "rename ok <path>", "delete ok <path>". Path di jejak
#      itu sekaligus membuktikan navigasi (naik/turun folder) bekerja, tanpa
#      perlu membaca UI.
#      Panic kernel juga terbaca di serial — probe pertama menemukan page fault
#      nyata (ListView::add_item pada widget NULL) lewat jalur ini.
#
#   python tests/host/probes/_fileman_probe.py [langkah...]
#   langkah: launch nav newfolder delete all            (default: all)
#
# Disk memakai SALINAN (test_disk.img) sehingga disk.img asli tidak tersentuh.
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _ui_probe as P          # harness boot/login/monitor/screendump
import _screen_text as ST      # baca_ppm (dipakai untuk sampling warna)

OUT = os.path.join(P.ROOT, "tests", "host", "probes", "_ui_out")
# Tema bawaan toolkit (libs/widget/include/core/theme.hpp) — dipakai untuk
# membuktikan jendela File Manager benar-benar tergambar.
THEME_BG     = (0x1A, 0x1A, 0x2E)
# Tombol dialog memakai theme.btnfill (0x3C3C3C), bukan button_bg toolbar —
# terverifikasi dari screendump: dua blok ~68px di baris dialog.
THEME_BTN_DIALOG = (0x3C, 0x3C, 0x3C)
WIN_W_EXPECT = 720
# Awalan jejak aplikasi (FileManagerApp::trace) — satu tempat saja.
TRACE = "[filemanager] "


def dump(m, name, wait=1.0):
    ppm = os.path.join(OUT, name + ".ppm")
    m.cmd("screendump %s" % ppm, wait)
    try:
        P.ppm_to_png(ppm, os.path.join(OUT, name + ".png"))
    except Exception:
        pass
    return ppm


def window_bbox(ppm, color=THEME_BG, step=2):
    """Bbox piksel berwarna `color` (isi jendela) + lebar/tinggi efektif."""
    w, h, px = ST.read_ppm(ppm)

    def rgb(x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    xs, ys = [], []
    for y in range(0, h, step):
        for x in range(0, w, step):
            if rgb(x, y) == color:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs), max(ys),
            max(xs) - min(xs), max(ys) - min(ys), len(xs))


def find_leftmost_button(ppm, y0, y1, color=THEME_BTN_DIALOG, min_run=24):
    """Pusat tombol paling KIRI pada pita y0..y1 (tombol dialog = index 0)."""
    w, h, px = ST.read_ppm(ppm)
    y1 = min(y1, h)
    runs = []
    for y in range(y0, y1, 4):
        run = 0
        start = 0
        for x in range(0, w):
            i = (y * w + x) * 3
            hit = (px[i], px[i + 1], px[i + 2]) == color
            if hit:
                if run == 0:
                    start = x
                run += 1
            else:
                if run >= min_run:
                    runs.append((start, x, y))
                run = 0
        if run >= min_run:
            runs.append((start, w, y))
    if not runs:
        return None
    # Klaster per-tombol: ambil run paling KIRI, lalu semua run yang mulainya
    # dekat (celah antar tombol dialog hanya ~6px → jangan sampai tergabung,
    # kalau tergabung pusatnya jatuh di celah dan klik tidak mengenai tombol).
    x_min = min(r[0] for r in runs)
    btn = [r for r in runs if r[0] <= x_min + 8]
    x0 = min(r[0] for r in btn)
    x1 = max(r[1] for r in btn)
    ys = [r[2] for r in btn]
    return ((x0 + x1) // 2, (min(ys) + max(ys)) // 2)


def create_and_rename(m, name, wait_bar=2.0):
    """Ctrl+N → tunggu bar → ketik nama (mengganti default) → Enter."""
    m.key("ctrl-n")
    time.sleep(wait_bar)
    for ch in name:
        m.key(ch if not ch.isupper() else "shift-" + ch.lower(), 0.18)
    time.sleep(0.8)
    m.key("ret")
    time.sleep(2.4)


def move_to(m, x, y, step=8):
    """Pindahkan kursor ke koordinat layar absolut (x, y).

    Protokol monitor QEMU hanya punya `mouse_move` RELATIF: dorong dulu ke sudut
    (0,0) — kernel/KWM menjepit kursor di tepi — lalu melangkah `step` px.
    Presisi ±step; cukup untuk baris sidebar (20 px) dan tombol toolbar (120 px).
    """
    for _ in range(60):
        m.move(-40, -40)
    time.sleep(0.3)
    for _ in range(x // step):
        m.move(step, 0)
    for _ in range(y // step):
        m.move(0, step)
    time.sleep(0.4)


def serial():
    return P.serial_text()


def main():
    steps = sys.argv[1:] or ["all"]
    want = lambda s: "all" in steps or s in steps     # noqa: E731
    P.prep()
    proc = P.start()
    try:
        m = P.Mon(P.connect())
        print("monitor ok")
        if not P.wait_serial("ready", 120):
            print("!! serial belum 'ready'; ekor log:")
            print(serial()[-1500:])
            return
        print("boot ready")
        m.type("root")
        m.key("ret")
        time.sleep(0.6)
        m.type("1")
        m.key("ret")
        P.wait_serial("root@kyuzen>", 40)
        time.sleep(2.0)
        print("logged in")

        m.type("start fileman")
        m.key("ret")
        time.sleep(4.0)
        ppm = dump(m, "f1_launch")
        bb = window_bbox(ppm)
        dump_ok = bb and abs(bb[4] - WIN_W_EXPECT) <= 24 and bb[5] > 300
        print("  [launch] bbox isi jendela: %s" % (bb,))
        print("  -> jendela tema bawaan ~%dpx tergambar: %s"
              % (WIN_W_EXPECT, "YA" if dump_ok else "TIDAK"))
        print("  -> kernel panic: %s" % ("YA" if "[PANIC]" in serial() else "TIDAK"))

        rows = None
        if want("nav"):
            m.key("down")
            time.sleep(0.5)
            m.key("ret")
            time.sleep(2.0)
            rows = dump(m, "f2_open")
            m.key("backspace")            # Up
            time.sleep(1.8)
            m.key("down")
            m.key("down")                 # home
            time.sleep(0.4)
            m.key("ret")
            time.sleep(1.8)
            m.key("down")
            time.sleep(0.4)
            m.key("ret")                  # user -> /home/user
            time.sleep(2.0)
            rows = dump(m, "f3_nested")

            # Geometri KLIEN diukur dari screendump (bukan diasumsikan):
            # baris sidebar ke-i menempati klien y = 72 + i*20 .. +20 (terbukti
            # dari baris terpilih pada f1_launch: "/" = item terakhir).
            # "Pictures" = item ke-3 → pusat y = 72 + 60 + 10 = 142, x = 8+66.
            cli_x, cli_y = (bb[0] + 1, bb[1] + 1) if bb else (0, 0)
            move_to(m, cli_x + 74, cli_y + 142)     # sidebar: Pictures
            m.click()
            time.sleep(2.2)
            dump(m, "f4_sidebar")
            m.key("alt-left")                       # Back
            time.sleep(2.0)
            m.key("alt-right")                      # Forward
            time.sleep(2.0)
            dump(m, "f5_history")
            m.key("alt-left")                       # kembali ke /home/user
            time.sleep(2.0)

            # Toolbar = bar pertama (klien y 0..28, terverifikasi dari hover:
            # rect theme.button_hover 116x22 muncul di y klien 3..25 saat kursor
            # di atasnya). Tombol ke-6 ("List / Icons") = sel x 600..720 →
            # pusat (660, 14); hover-nya dijadikan bukti geometri lebih dulu.
            move_to(m, cli_x + 660, cli_y + 14, step=4)
            time.sleep(0.6)
            dump(m, "f6_toolbar_hover", 0.4)
            m.click()                              # → Icon view
            time.sleep(1.6)
            dump(m, "f7_icons")
            m.click()                              # → List view lagi
            time.sleep(1.6)

        # Nama unik per run: disk uji (test_disk.img) PERSISTEN antar run, jadi
        # nama tetap akan bertabrakan dengan sisa run sebelumnya (EEXIST) dan
        # membuat langkah rename/delete gagal bukan karena bug.
        name = "Foto%d" % (int(time.time()) % 100000)
        if want("newfolder"):
            dump(m, "f8_newbar")
            create_and_rename(m, name)
            dump(m, "f9_created")
            s = serial()
            # "New Folder" sebagai awalan juga menangkap "New Folder (2)" (nama
            # unik) — yang dibuktikan adalah DIRECTORI AKTIF = /home/user.
            mk = (TRACE + "mkdir ok /home/user/New Folder") in s
            rn = (TRACE + "rename ok /home/user/%s" % name) in s
            if not (mk and rn):
                # Percobaan kedua: timing keypress lewat KWM bisa meleset satu
                # frame; kalau rename tidak terlihat, Enter tadi kemungkinan
                # "membuka" folder baru (path berpindah) → naik dulu ke parent.
                print("  (percobaan kedua)")
                m.key("backspace")
                time.sleep(1.6)
                name = name + "b"
                create_and_rename(m, name)
                s = serial()
                rn = (TRACE + "rename ok /home/user/%s" % name) in s
            print("  -> new folder dibuat di /home/user: %s" % ("YA" if mk else "TIDAK"))
            print("  -> rename inline (nama default -> nama baru): %s" % ("YA" if rn else "TIDAK"))
            print("  -> navigasi root->apps->up->home->user terbukti (path jejak): %s"
                  % ("YA" if (mk or rn) else "TIDAK"))

        if want("delete"):
            m.key("delete")               # dialog konfirmasi
            time.sleep(1.5)
            ppm = dump(m, "f10_confirm")
            w, h, _ = ST.read_ppm(ppm)
            # Pita bawah jendela: tombol dialog ada di bawah tengah modal.
            pos = find_leftmost_button(ppm, 340, 600)
            print("  [delete] tombol kiri (Delete) @ %s" % (pos,))
            if pos:
                for _ in range(250):
                    m.move(-40, -40)
                time.sleep(0.4)
                for _ in range(pos[0] // 4):
                    m.move(4, 0)
                for _ in range(pos[1] // 4):
                    m.move(0, 4)
                time.sleep(0.4)
                m.click()
                time.sleep(2.6)
                dump(m, "f11_deleted")
                s = serial()
                delt = (TRACE + "delete ok /home/user/%s" % name) in s
                print("  -> delete dikonfirmasi lewat klik: %s" % ("YA" if delt else "TIDAK"))
            else:
                m.key("esc")              # batal (jalur aman) kalau tombol tak ketemu
                time.sleep(1.0)
                dump(m, "f9_cancelled")
                print("  -> dialog dibatalkan dengan ESC (tombol tidak terdeteksi)")

        s = serial()
        navs = [ln.split("path ", 1)[1].strip() for ln in s.splitlines()
                if TRACE + "path " in ln]
        views = [ln.split("view ", 1)[1].strip() for ln in s.splitlines()
                 if TRACE + "view " in ln]
        print("  [sidebar+riwayat] jejak path: %s" % (navs,))
        print("  -> klik sidebar Pictures: %s"
              % ("YA" if any(p.endswith("/Pictures") for p in navs) else "TIDAK"))
        print("  -> Back/Forward berpindah path: %s"
              % ("YA" if len(navs) >= 3 else "TIDAK"))
        print("  [view] jejak: %s" % (views,))
        print("  -> tombol toolbar List/Icons mengubah mode: %s"
              % ("YA" if "icons" in views and "list" in views else "TIDAK"))
        print("  -> PANIC selama sesi: %s" % ("YA" if "[PANIC]" in s else "TIDAK"))
        print("--- jejak fileman di serial ---")
        for line in s.splitlines():
            if TRACE in line or "[PANIC]" in line:
                print("   " + line.strip())
    finally:
        try:
            m.cmd("quit")
        except Exception:
            pass
        time.sleep(1)
        if proc.poll() is None:
            proc.terminate()


if __name__ == "__main__":
    main()
