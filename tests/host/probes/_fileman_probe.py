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

# Harness _ui_probe.py menunjuk build/boot_image.iso (path Makefile lama).
# Build CMake menaruh ISO di build/target/boot_image.iso — pilih yang ada,
# supaya probe jalan sebelum dan sesudah migrasi path.
_ISO_CMAKE = os.path.join(P.ROOT, "build", "target", "boot_image.iso")
if os.path.exists(_ISO_CMAKE):
    P.ISO = _ISO_CMAKE

OUT = os.path.join(P.ROOT, "tests", "host", "probes", "_ui_out")

# ---------------------------------------------------------------------------
# Geometri + warna dari DESIGN SYSTEM (bukan angka yang disalin dari kode app).
#
# Sumber: libs/gui/widget/include/theme/{colors,metrics}.hpp
#   bg (Dark+Neutral)        0x111111   <- isi jendela aplikasi
#   surface                  0x181818   <- pita chrome (menubar/toolbar/statusbar)
#   surface_elevated         0x232323   <- tombol sekunder
#   accent (Neutral)         0x8B8B8B   <- tombol primer (dialog)
#   menubar 26 / toolbar 34 / statusbar 24
#
# CATATAN PENTING soal identifikasi jendela: bg aplikasi (0x111111) TIDAK bisa
# dipakai sebagai penanda jendela — shell desktop memakai surface 0x181818, dan
# setelah redesign keduanya netral, sehingga pencarian "piksel 0x111111" juga
# mengenai area gelap lain di layar. Yang dipakai penanda adalah PITA CHROME
# (0x181818) yang membentuk run horizontal panjang selebar jendela: itulah
# menubar/toolbar yang hanya dimiliki jendela aplikasi.
# ---------------------------------------------------------------------------
THEME_BG = (0x11, 0x11, 0x11)             # isi jendela aplikasi
THEME_CHROME = (0x18, 0x18, 0x18)         # pita chrome (menubar/toolbar/statusbar)
THEME_BTN_DIALOG = (0x23, 0x23, 0x23)     # surface_elevated
THEME_BTN_PRIMARY = (0x8B, 0x8B, 0x8B)    # accent (Neutral)
WIN_W_EXPECT = 720

MENUBAR_H = 26
TOOLBAR_H = 34
STATUSBAR_H = 24
ROOT_Y = 8 + MENUBAR_H + TOOLBAR_H        # 68 (relatif ke atas klien)
CONTROL_H = 28
SPACE_SM = 8
ROW_H = 24                                # list::ROW_H
SIDEBAR_X = 8
SIDEBAR_W = 140

# Baris sidebar ke-i (klien-relative): tinggi ROW_H, mulai di bawah baris path.
SIDEBAR_TOP = ROOT_Y + CONTROL_H + SPACE_SM      # 104
TOOLBAR_MID = MENUBAR_H + TOOLBAR_H // 2         # 43

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


def window_bbox(ppm, color=THEME_CHROME, min_run=300, step=2):
    """Bbox jendela aplikasi dari PITA CHROME-nya.

    Menubar/toolbar adalah satu-satunya elemen di layar yang membentuk run
    horizontal panjang berwarna `surface` selebar jendela. Dipakai itu sebagai
    penanda, bukan bg aplikasi: bg (0x111111) juga muncul di area gelap lain
    setelah shell dan aplikasi memakai palet netral yang sama.

    Return (x0, y0, x1, y1, lebar, tinggi, jumlah_piksel) atau None.
    """
    w, h, px = ST.read_ppm(ppm)

    def rgb(x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    # Baris mana yang memuat run chrome panjang? Itu pita chrome jendela.
    # Taskbar juga memakai `surface`, jadi run panjang saja tidak cukup:
    # taskbar membentang SELEBAR LAYAR (1920), sedangkan menubar/toolbar
    # selebar JENDELA (~720). Karena itu kandidat dikelompokkan per LEBAR run
    # dan kelompok dengan lebar paling masuk akal (<= WIN_W_EXPECT + margin,
    # dan bukan selebar layar) yang dipakai.
    candidates = []          # (y, bx, run_len)
    for y in range(0, h):
        run = 0
        best = 0
        bx = 0
        for x in range(0, w):
            if rgb(x, y) == color:
                run += 1
                if run > best:
                    best = run
                    bx = x - run + 1
            else:
                run = 0
        if best >= min_run:
            candidates.append((y, bx, best))
    if not candidates:
        return None

    # Buang run yang selebar layar penuh (itu taskbar, bukan jendela).
    not_full = [c for c in candidates if c[2] <= WIN_W_EXPECT + 24]
    if not_full:
        candidates = not_full

    x0 = min(c[1] for c in candidates)
    x1 = max(c[1] + c[2] - 1 for c in candidates)
    y0 = min(c[0] for c in candidates)
    y1 = max(c[0] for c in candidates)
    width = x1 - x0 + 1
    return (x0, y0, x1, y1, width, y1 - y0 + 1, len(candidates))


def find_leftmost_button(ppm, y0, y1, colors=(THEME_BTN_PRIMARY, THEME_BTN_DIALOG),
                         min_run=24):
    """Pusat tombol paling KIRI pada pita y0..y1 (tombol dialog = index 0).
    Cari di semua warna kandidat (primer-aksen dulu, fallback btnfill lama);
    yang paling kiri menang — tombol primer selalu di kiri (konvensi OK/Batal)."""
    w, h, px = ST.read_ppm(ppm)
    y1 = min(y1, h)
    best = None
    for color in colors:
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
            continue
        # Klaster per-tombol: ambil run paling KIRI, lalu semua run yang mulainya
        # dekat (celah antar tombol dialog hanya ~6px → jangan sampai tergabung,
        # kalau tergabung pusatnya jatuh di celah dan klik tidak mengenai tombol).
        x_min = min(r[0] for r in runs)
        btn = [r for r in runs if r[0] <= x_min + 8]
        x0 = min(r[0] for r in btn)
        x1 = max(r[1] for r in btn)
        ys = [r[2] for r in btn]
        cand = ((x0 + x1) // 2, (min(ys) + max(ys)) // 2)
        if best is None or cand[0] < best[0]:
            best = cand
    return best


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
    (0,0) - kernel/KWM menjepit kursor di tepi - lalu melangkah `step` px.

    Sisa pembagian dikirim sebagai langkah TERAKHIR (bukan dipotong `//step`),
    dan jumlah dorongan slam disamakan dengan _ui_probe.py yang terbukti bekerja.
    """
    for _ in range(250):
        m.move(-40, -40)
    time.sleep(0.5)
    for _ in range(x // step):
        m.move(step, 0)
    if x % step:
        m.move(x % step, 0)
    for _ in range(y // step):
        m.move(0, step)
    if y % step:
        m.move(0, y % step)
    time.sleep(0.4)


def calibrate_cursor(m, ppm, cli_x, cli_y):
    """Ukur perpindahan kursor yang SEBENARNYA terjadi (skala harness).

    Protokol mouse monitor QEMU memakai koordinat layar HOST, sementara tamu
    (guest) punya resolusinya sendiri. Dengan `-display none` pemetaannya tidak
    identik 1:1, sehingga kursor mendarat dengan selisih tetap terhadap target.
    Alih-alih menebak faktor skalanya, probe MENGUKURNYA: dorong kursor ke
    sudut, maju sejauh D pada sumbu Y, klik baris sidebar, lalu baca baris mana
    yang benar-benar terpilih — dari situ skala = terukur / D.

    Return (offset, skala) dalam satuan klien, atau None bila tak terukur.
    """
    probe_d = 120                       # dorongan uji pada sumbu Y
    for _ in range(250):
        m.move(-40, -40)
    time.sleep(0.5)
    for _ in range(probe_d // 8):
        m.move(0, 8)
    time.sleep(0.3)
    m.click()
    time.sleep(1.5)
    fresh = dump(m, "f0_calib")
    got = selected_sidebar_row(fresh, cli_x, cli_y)
    if got is None:
        return None
    landed = SIDEBAR_TOP + got * ROW_H + ROW_H // 2
    # Kursor mendarat di `landed` padahal kita mendorong `probe_d` dari sudut.
    scale = landed / probe_d
    print("  [calib] dorong %d px -> mendarat di rel %d (baris %d), skala %.3f"
          % (probe_d, landed, got, scale))
    return (0, scale)


def move_to_scaled(m, x, y, scale=1.0, step=8):
    """move_to() dengan kompensasi skala kursor."""
    sx = int(round(x * scale))
    sy = int(round(y * scale))
    move_to(m, sx, sy, step)


def selected_sidebar_row(ppm, cli_x, cli_y):
    """Indeks baris sidebar yang sedang terpilih (tinta seleksi), atau None."""
    w, h, px = ST.read_ppm(ppm)

    def rgb(x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    # selection = mix(accent, bg, 77) -> 0x353535 untuk Neutral/dark.
    sel = (0x35, 0x35, 0x35)
    band = []
    for y in range(cli_y + SIDEBAR_TOP, cli_y + SIDEBAR_TOP + 8 * ROW_H):
        if y >= h:
            break
        if rgb(cli_x + 20, y) == sel:
            band.append(y)
    if not band:
        return None
    centre = (band[0] + band[-1]) // 2 - cli_y
    return (centre - SIDEBAR_TOP) // ROW_H


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

        # FOKUS JENDELA DULU. Klik mouse dirutekan KWM ke jendela yang
        # berfokus; tanpa langkah ini klik pertama hanya memfokuskan jendela
        # dan tidak sampai ke aplikasi.
        #
        # TITIK KLIK: area KONTEN yang kosong (di bawah baris daftar), BUKAN
        # menubar — mengklik menubar membuka menu, dan menu yang terbuka
        # menangkap seluruh input berikutnya (panah/Enter menggerakkan menu,
        # bukan aplikasi). JANGAN kirim ESC sesudahnya: di File Manager ESC
        # menutup window (itu perilaku yang diinginkan aplikasi, tapi mematikan
        # sisa probe).
        bb0 = window_bbox(ppm)
        if bb0:
            focus_x = bb0[0] + 400
            focus_y = bb0[1] + 470          # dekat dasar konten, area kosong
            move_to(m, focus_x, focus_y)
            m.click()
            time.sleep(1.0)
            print("  [launch] klik fokus pada area konten (%d,%d)"
                  % (focus_x, focus_y))

        bb = window_bbox(ppm)
        # Lebar jendela HARUS ~720 (menubar/toolbar membentang selebar klien).
        # Tinggi bbox hanya mencakup baris yang memuat run chrome panjang, jadi
        # yang diperiksa tinggi adalah "ada banyak baris pita", bukan tinggi
        # jendela penuh.
        dump_ok = bb and abs(bb[4] - WIN_W_EXPECT) <= 24 and bb[6] >= MENUBAR_H
        print("  [launch] bbox pita chrome jendela: %s" % (bb,))
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
            # JANGAN tekan `ret` pada entri acak: berkas tanpa handler
            # memunculkan dialog modal "No application is associated", dan
            # modal menangkap SEMUA input berikutnya sehingga langkah
            # sesudahnya diam-diam tidak sampai ke aplikasi.
            # JANGAN pakai ESC untuk menutupnya juga: di File Manager ESC
            # adalah "tutup window" (perilaku aplikasi yang benar), jadi ESC
            # di sini mematikan sisa probe.
            # Cukup kembali ke root dengan Backspace (Up) berulang.
            for _ in range(4):
                m.key("backspace")
                time.sleep(0.6)
            rows = dump(m, "f3_nested")

            # Geometri KLIEN diukur dari screendump (bukan diasumsikan): bbox
            # memberi origin klien, dan posisi baris dihitung dari TOKEN tema
            # (SIDEBAR_TOP + i*ROW_H).
            #
            # BARIS mana yang diklik diambil dari JEJAK APLIKASI
            # ("[filemanager] shortcuts Home,Documents,..."), bukan dari
            # asumsi. Sidebar hanya memuat direktori yang BENAR-BENAR ada, jadi
            # urutannya berbeda antar disk — mengasumsikan "Pictures = baris
            # ke-3" membuat probe gagal pada disk tanpa /home/user.
            cli_x, cli_y = (bb[0], bb[1]) if bb else (0, 0)
            labels = []
            for line in serial().splitlines():
                if TRACE + "shortcuts " in line:
                    labels = line.split("shortcuts ", 1)[1].strip().split(",")
            print("  [sidebar] shortcut terdeteksi: %s" % (labels,))
            target = "Pictures" if "Pictures" in labels else (
                labels[1] if len(labels) > 1 else (labels[0] if labels else None))
            if target:
                # Sidebar lewat KEYBOARD (panah kanan = shortcut berikutnya).
                # Jalur ini deterministik: tidak bergantung pada pemetaan
                # koordinat mouse harness, yang di `-display none` tidak 1:1
                # dengan guest dan skalanya terbukti tidak linear.
                #
                # CATATAN: cycleShortcut() mulai dari shortcut yang cocok
                # dengan path AKTIF; kalau path sekarang bukan salah satu
                # shortcut, ia mulai dari indeks 0. Karena itu probe mencari
                # sampai path-nya berakhir dengan nama target, bukan berhenti
                # setelah satu langkah.
                pos_before = len(serial())
                found = False
                for _ in range(len(labels) + 1):
                    m.key("right")
                    time.sleep(0.7)
                    if ("path /" in serial()[pos_before:]
                            and target in serial()[pos_before:]):
                        found = True
                        break
                got_path = ""
                for line in serial().splitlines():
                    if TRACE + "path " in line:
                        got_path = line.split("path ", 1)[1].strip()
                if got_path.endswith("/" + target):
                    print("  -> sidebar (keyboard): navigasi ke '%s' berhasil"
                          % target)
                else:
                    print("  -> sidebar (keyboard): path terakhir '%s' (target %s, found=%s)"
                          % (got_path, target, found))
            else:
                print("  -> sidebar: tidak ada shortcut (disk kosong?) — dilewati")
            m.key("alt-left")                       # Back
            time.sleep(2.0)
            m.key("alt-right")                      # Forward
            time.sleep(2.0)
            dump(m, "f5_history")
            m.key("alt-left")                       # kembali
            time.sleep(2.0)

            # Toolbar = pita kedua (klien y MENUBAR_H..MENUBAR_H+TOOLBAR_H).
            # Tombol "Icons" (toggle view) adalah tombol TERAKHIR; pusatnya
            # dihitung dari token (ikon + teks + padding) supaya probe tidak
            # bergantung pada lebar hardcoded dari implementasi lama.
            def toolbar_btn_center(labels, want):
                """Pusat x tombol `want` dalam barisan label toolbar."""
                x = 4                                   # space::XS
                for lbl in labels:
                    w = 16 + 8 + len(lbl) * 8 + 16       # ikon+gap+teks+2*pad
                    if lbl == want:
                        return x + w // 2
                    x += w + 4
                return None
            tb_labels = ["Back", "Forward", "Up", "Refresh", "New Folder", "Icons"]
            tb_cx = toolbar_btn_center(tb_labels, "Icons")
            if tb_cx is None:
                tb_cx = 40          # jaga-jaga: jatuh ke tombol pertama
            move_to(m, cli_x + tb_cx, cli_y + TOOLBAR_MID, step=4)
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
