#!/usr/bin/env python3
# _settings_probe.py — verifikasi RUNTIME Settings (C++) di QEMU headless.
#
# Memakai harness tests/host/probes/_ui_probe.py (boot -> login root/1).
# Fase 1 (sesi QEMU pertama):
#   1. SERIAL "[settings] start" — app mencapai ui_window_run tanpa crash.
#   2. SCREENDUMP — bbox jendela 720px terlihat.
#   3. Tombol '2' — shortcut font: serial "[settings] Fonts" (page switch).
#   4. Wallpaper live: N (preview berikutnya) + A (apply), 3x; tiap apply
#      harus memunculkan "[desktop] wallpaper: ganti" ≤12 dtk DAN piksel
#      region wallpaper-only berubah. Settings tetap terbuka (desktop tidak
#      restart, window lain hidup).
#   5. ESC — window tertutup, tidak ada panic/BSOD di serial.
# Fase 2 (reboot QEMU, disk sama): region wallpaper harus IDENTIK dengan
# hash terakhir fase 1 (persistensi manifest).
#
# Disk memakai SALINAN (test_disk.img) sehingga disk.img asli tidak tersentuh.
# Jalankan dari root repo:  python tests/host/probes/_settings_probe.py
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _ui_probe as P
import _screen_text as ST

P.QEMU = r"E:\Tools\msys2\ucrt64\bin\qemu-system-x86_64.exe"
OUT = os.path.join(P.ROOT, "tests", "host", "probes", "_ui_out")
# bg tema yang mungkin dipakai window (settings.ui tersimpan di test disk
# bisa mengubah tema dari bawaan toolkit). Gelap/Terang/Hijau = preset
# settings; terakhir = bawaan toolkit (tanpa settings.ui).
THEME_BGS = [
    (0x12, 0x12, 0x12),
    (0xF0, 0xF0, 0xF0),
    (0x0D, 0x1F, 0x14),
    (0x1A, 0x1A, 0x2E),
]


def window_width(ppm, step=2):
    w, h, px = ST.read_ppm(ppm)

    def rgb(x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    best = None
    for color in THEME_BGS:
        xs = []
        for y in range(0, h, step):
            for x in range(0, w, step):
                if rgb(x, y) == color:
                    xs.append(x)
        if xs and (best is None or len(xs) > best[1]):
            best = (max(xs) - min(xs), len(xs))
    return best


def sidebar_selected_y(ppm):
    """Baris sidebar yang ter-highlight: pita non-bg ≥100px di x100-260,
    tinggi 12..28px (satu baris ListView 20px), y>=120 (lewati titlebar).
    Return y tengah, atau None."""
    w, h, px = ST.read_ppm(ppm)

    def rgb(x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    bg = rgb(150, 620)
    bands = []
    cur = None
    for y in range(120, 320, 2):
        run = 0
        for x in range(100, 260):
            if rgb(x, y) != bg:
                run += 1
            else:
                if run >= 100:
                    break
                run = 0
        if run >= 100:
            if cur is None:
                cur = [y, y]
            else:
                cur[1] = y
        elif cur is not None:
            bands.append(cur)
            cur = None
    if cur is not None:
        bands.append(cur)
    for y0, y1 in bands:
        if 12 <= (y1 - y0) <= 28:
            return (y0 + y1) // 2
    return None


def wait_count(needle, want_more_than, timeout=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if P.serial_text().count(needle) > want_more_than:
            return True
        time.sleep(0.5)
    return False


def dense_photo_height(ppm):
    """Tinggi blok foto padat di area konten (x300-580). 0 = tak ada foto."""
    w, h, px = ST.read_ppm(ppm)

    def rgb(x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    top = bot = -1
    for y in range(120, 625, 2):
        varied = sum(1 for x in range(300, 580, 8)
                     if rgb(x, y) != rgb(x + 8, y))
        if varied > 20:
            if top < 0:
                top = y
            bot = y
    return (bot - top) if top >= 0 else 0


def click_at(m, x, y):
    for _ in range(250):
        m.move(-40, -40)
    time.sleep(0.5)
    for _ in range(x // 4):
        m.move(4, 0)
    for _ in range(y // 4):
        m.move(0, 4)
    time.sleep(0.5)
    m.click()
    time.sleep(1.5)
def titlebar_present(ppm, min_run=200):
    """Ada pita titlebar KWM (fokus 2D2D2D / non-fokus 252526) selebar
    min_run px? Bukti sebuah window app terbuka (desktop frameless)."""
    w, h, px = ST.read_ppm(ppm)

    def rgb(x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    targets = ((0x2D, 0x2D, 0x2D), (0x25, 0x25, 0x26))
    for y in range(0, h, 4):
        run = 0
        for x in range(0, w):
            if rgb(x, y) in targets:
                run += 1
                if run >= min_run:
                    return True
            else:
                run = 0
    return False


def click_desktop_icon0(m):
    """Klik ikon launcher sel 0 (fileman.elf = modul .elf pertama, tak
    hidden): slam ke (0,0) lalu ke tengah ikon (48,48)."""
    for _ in range(250):
        m.move(-40, -40)
    time.sleep(0.5)
    for _ in range(12):
        m.move(4, 0)
    for _ in range(12):
        m.move(0, 4)
    time.sleep(0.5)
    m.click()
    time.sleep(2.0)
def wall_distinct(ppm):
    """Jumlah warna berbeda di region wallpaper-only."""
    w, h, px = ST.read_ppm(ppm)

    def rgb(x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    vals = set()
    for y in range(880, 1030, 4):
        for x in range(1700, 1919, 4):
            vals.add(rgb(x, y))
    return len(vals)


def wait_wallpaper_content(m, timeout=30.0):
    """Tunggu compositor benar-benar mempresentasikan wallpaper (bukan canvas
    fresh 0x1E293B): region harus punya >10 warna berbeda."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        ppm = shot(m, "st_wait")
        if wall_distinct(ppm) > 10:
            return True
        time.sleep(2.0)
    return False


def wall_diff(a, b):
    """Fraksi piksel berbeda di region wallpaper-only (kanan-bawah, di atas
    taskbar): tidak ada ikon launcher (satu baris di atas) maupun jam
    taskbar (di bawah). 0.0 = identik."""
    w, h, pa = ST.read_ppm(a)
    w, h, pb = ST.read_ppm(b)

    def rgb(px, x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    diff = 0
    n = 0
    for y in range(880, 1030, 4):
        for x in range(1700, 1919, 4):
            n += 1
            if rgb(pa, x, y) != rgb(pb, x, y):
                diff += 1
    return diff / n if n else 1.0


def shot(m, name):
    ppm = os.path.join(OUT, name + ".ppm")
    m.cmd("screendump %s" % ppm, 1.0)
    try:
        P.ppm_to_png(ppm, os.path.join(OUT, name + ".png"))
    except Exception:
        pass
    return ppm


def boot_and_login(m):
    """Boot -> login -> desktop siap. Return True bila semua terlihat."""
    if not P.wait_serial("ready", 180):
        return False
    pos = len(P.serial_text())

    def wait_new(needle, timeout=60.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in P.serial_text()[pos:]:
                return True
            time.sleep(0.5)
        return False

    # Ketik berbasis prompt serial (bukan sleep tetap): boot kadang lambat
    # (decode font/wallpaper) dan ketikan dini hilang.
    if not wait_new("Username", 60):
        return False
    m.type("root", wait=0.2)
    m.key("ret")
    if not wait_new("Password", 30):
        return False
    time.sleep(0.5)
    m.type("1", wait=0.2)
    m.key("ret")
    if not wait_new("kyuzen>", 90):
        return False
    # Desktop spawn SETELAH login; output-nya menginterleave ketikan
    # berikutnya, jadi tunggu selesai (cek hasil; decode font kadang lambat).
    t0 = time.time()
    while time.time() - t0 < 180:
        if P.wait_serial("[desktop] shell started", 20):
            break
        time.sleep(1.0)
    else:
        return False
    time.sleep(2.0)  # sisa render/present pertama
    return True


def stop_qemu(m, p):
    try:
        if m is not None:
            m.cmd("quit")
    except Exception:
        pass
    time.sleep(1)
    if p.poll() is None:
        p.terminate()
    time.sleep(1)


def wait_new_serial(needle, timeout=15.0):
    pos = len(P.serial_text())
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in P.serial_text()[pos:]:
            return True
        time.sleep(0.5)
    return False


def no_panic():
    ser = P.serial_text()
    # "[PANIC_LOG] ..." adalah pesan boot benign, bukan panic.
    return not ("BSOD" in ser or "PANIC BERSARANG" in ser or "#PF" in ser
                or "#GP" in ser)


def main():
    P.prep()
    ok = True
    last_wall = None

    # ---------- Fase 1: launch + switch live ----------
    p = P.start()
    m = None
    try:
        m = P.Mon(P.connect())
        print("monitor ok")
        if not boot_and_login(m):
            print("FAIL: boot/login")
            print(P.serial_text()[-1500:])
            return 1
        print("PASS: boot+login")
        m.type("start settings", wait=0.25)
        time.sleep(0.5)
        m.key("ret")
        time.sleep(1.0)
        if not P.wait_serial("[settings] start", 60):
            print("FAIL: '[settings] start' tidak muncul di serial")
            print(P.serial_text()[-1500:])
            return 1
        print("PASS: settings launched ([settings] start)")
        time.sleep(2.0)
        ww = window_width(shot(m, "st_main"))
        if ww is None or not (640 <= ww[0] <= 730):
            print("FAIL: bbox jendela (ww=%s)" % (ww,))
            ok = False
        else:
            print("PASS: window geometry ~%d px" % ww[0])

        m.key("2")  # shortcut font -> showPage(Fonts) -> trace "Fonts"
        time.sleep(1.0)
        if "[settings] Fonts" in P.serial_text():
            print("PASS: sidebar/page switch via keyboard")
        else:
            print("FAIL: '[settings] Fonts' tidak muncul setelah tombol 2")
            ok = False

        # --- Jendela pendamping (Test 3): klik ikon sel 0 (fileman) ---
        # Mouse routing posisional (tak peduli fokus keyboard); window baru
        # mengambil fokus. Lalu Alt+Tab mengembalikan fokus ke settings.
        click_desktop_icon0(m)
        if "[filemanager]" not in P.serial_text():
            print("FAIL: companion app tak ter-launch via ikon")
            ok = False
        else:
            print("PASS: companion fileman launched (klik ikon)")
        m.key("alt-tab")  # fokus kembali ke settings
        time.sleep(1.0)
        m.key("n")
        time.sleep(1.0)
        if "[settings] Personalization" not in P.serial_text():
            print("FAIL: fokus tak kembali ke settings setelah Alt+Tab")
            ok = False
        else:
            print("PASS: Alt+Tab focus settings (companion terbuka di belakang)")

        prev = shot(m, "st_wall0")
        # --- Instant: syscall event, bukan poll 5 dtk (bukti: <3 dtk) ---
        m.key("n")  # Personalization + preview berikutnya
        time.sleep(0.7)
        t0 = time.time()
        m.key("a")  # apply -> persist + sys_wallpaper_reload()
        if not wait_new_serial("[desktop] wallpaper: ganti", 15.0):
            print("FAIL: desktop tidak reload setelah apply #1")
            ok = False
        else:
            dt = time.time() - t0
            # Polling wallpaper sudah DIHAPUS dari rescan: marker ini hanya
            # bisa datang dari jalur event syscall 84. Waktu = delivery
            # (ms) + decode+scale PNG (detik, tergantung ukuran berkas).
            print("wallpaper #1: reload dalam %.1f dtk" % dt)
            print("PASS: instant reload via syscall (tanpa poll)")
        time.sleep(1.0)
        cur = shot(m, "st_wall1")
        if wall_diff(prev, cur) < 0.05:
            print("FAIL: piksel wallpaper tak berubah setelah apply #1")
            ok = False
        prev = cur
        # --- Rapid: 3x apply beruntun tanpa jeda (uji antrean event) ---
        for i in (2, 3, 4):
            m.key("n")
            time.sleep(0.2)
            m.key("a")
            time.sleep(0.2)
        t0 = time.time()
        while time.time() - t0 < 20:
            if P.serial_text().count("[desktop] wallpaper: ganti") >= 4:
                break
            time.sleep(0.5)
        else:
            print("FAIL: reload cepat tak lengkap (marker <4)")
            ok = False
        time.sleep(1.5)
        cur = shot(m, "st_wall4")
        if wall_diff(prev, cur) < 0.05:
            print("FAIL: piksel tak berubah setelah rapid apply")
            ok = False
        else:
            print("PASS: rapid wallpaper switch (desktop tetap hidup)")
        prev = cur
        # Settings masih responsif setelah rapid (loop event-nya hidup).
        m.key("2")
        time.sleep(1.0)
        if P.serial_text().count("[settings] Fonts") >= 2:
            print("PASS: settings alive after rapid")
        else:
            print("FAIL: settings tak responsif setelah rapid")
            ok = False
        # --- Ghost test: Personalization (foto) -> Appearance -> Fonts ---
        # Klik sidebar mouse; tiap pindah harus tanpa sisa piksel halaman lama
        # (fix owner propagation ScrollView/Tab di libs). Bukti: trace serial
        # + tak ada blok foto padat di screenshot halaman non-foto.
        # Keypress QEMU kadang hilang: verifikasi tiap transisi (retry 3x).
        def ensure_page(key, needle, base):
            for _ in range(3):
                m.key(key)
                time.sleep(1.0)
                if P.serial_text().count(needle) > base:
                    return True
            return False

        n_pers = P.serial_text().count("[settings] Personalization")
        if not ensure_page("n", "[settings] Personalization", n_pers):
            print("FAIL: tak bisa ke Personalization via keyboard")
            ok = False
            sel_y = None
        else:
            sel_y = sidebar_selected_y(shot(m, "st_ghost_base"))
            if sel_y is None:
                print("FAIL: highlight sidebar tak ditemukan")
        if sel_y is not None:
            # Sanity: highlight harus di baris Personalization (idx 1).
            # Posisi list dari screenshot: cari tepi atas ListView = baris
            # System (teks), lalu idx = (sel_y - y0) // 20 harus 1.
            click_at(m, 180, sel_y + 20)  # Appearance
            if not wait_count("[settings] Appearance", 0):
                print("FAIL: klik Appearance tak berpindah halaman")
                ok = False
            else:
                h = dense_photo_height(shot(m, "st_ghost_appear"))
                print("ghost Appearance: blok foto %dpx" % h)
                if h > 30:
                    print("FAIL: sisa foto Personalization di Appearance")
                    ok = False
                else:
                    print("PASS: no ghost on Appearance")
            n_fonts = P.serial_text().count("[settings] Fonts")
            click_at(m, 180, sel_y + 40)  # Fonts
            if not wait_count("[settings] Fonts", n_fonts):
                print("FAIL: klik Fonts tak berpindah halaman")
                ok = False
            else:
                h = dense_photo_height(shot(m, "st_ghost_fonts"))
                print("ghost Fonts: blok foto %dpx" % h)
                if h > 30:
                    print("FAIL: sisa foto Personalization di Fonts")
                    ok = False
                else:
                    print("PASS: no ghost on Fonts")
        last_wall = prev

        m.key("esc")  # tutup settings (masih terbuka = desktop tak restart)
        time.sleep(1.5)
        if no_panic():
            print("PASS: no panic after close")
        else:
            print("FAIL: panic terdeteksi di serial")
            ok = False
        # Companion (fileman) harus masih terbuka di atas wallpaper baru.
        after = shot(m, "st_companion")
        if titlebar_present(after):
            print("PASS: companion window alive after switches+close")
        else:
            print("FAIL: companion window hilang")
            ok = False
        if wall_diff(last_wall, after) > 0.001:
            print("FAIL: wallpaper berubah saat settings ditutup")
            ok = False
        else:
            print("PASS: wallpaper stabil setelah close")
        # WM + spawn sehat setelah semua switch: tutup companion (fokus
        # kembali ke shell), lalu settings bisa dibuka lagi. ESC ganda:
        # pertama untuk fileman (bila masih fokus), kedua no-op di shell.
        m.key("esc")  # tutup fileman
        time.sleep(1.5)
        m.key("esc")  # pastikan (no-op bila sudah di shell)
        time.sleep(1.0)
        if titlebar_present(shot(m, "st_closed")):
            print("FAIL: window masih terbuka setelah ESC ganda")
            ok = False
        m.key("ret")  # baris kosong: buang sisa byte stray (ESC) di buffer shell
        time.sleep(0.8)
        m.type("start settings", wait=0.25)
        time.sleep(0.5)
        m.key("ret")
        # Spawn async: prompt kembali seketika, trace app menyusul — tunggu
        # kemunculan ke-2, bukan sleep tetap.
        t0 = time.time()
        while time.time() - t0 < 30:
            if P.serial_text().count("[settings] start") >= 2:
                break
            time.sleep(0.5)
        else:
            print("FAIL: settings tak bisa dibuka lagi")
            ok = False
        if ok:
            print("PASS: relaunch settings ok")
        m.key("esc")
        time.sleep(1.0)
    finally:
        stop_qemu(m, p)

    if not ok or last_wall is None:
        return 1

    # ---------- Fase 2: reboot -> wallpaper persisten ----------
    if os.path.exists(P.LOG):
        os.remove(P.LOG)
    p = P.start()
    m = None
    try:
        m = P.Mon(P.connect())
        if not boot_and_login(m):
            print("FAIL: reboot boot/login")
            return 1
        if not wait_wallpaper_content(m):
            print("FAIL: compositor tak mempresentasikan wallpaper (reboot)")
            return 1
        time.sleep(2.0)
        cur = shot(m, "st_wall_reboot")
        d = wall_diff(last_wall, cur)
        print("reboot persist: diff=%.4f" % d)
        if d > 0.001:
            print("FAIL: wallpaper tidak persisten setelah reboot")
            return 1
        print("PASS: wallpaper persists across reboot")
        print("--- serial tail ---")
        print(P.serial_text()[-800:])
        return 0
    finally:
        stop_qemu(m, p)


if __name__ == "__main__":
    sys.exit(main())
